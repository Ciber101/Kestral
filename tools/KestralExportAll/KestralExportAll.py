"""Kestral: move every Fusion design out in one go.

Run inside Fusion (Utilities > Scripts and Add-Ins > + > pick this folder > Run).
For EVERY design in EVERY project of every team hub you can see, it writes:
  * a STEP file Kestral opens (bodies, components and folders, names, colors)
  * a .kestral.json next to it listing the parts that were hidden (eye off) in Fusion, so
    Kestral hides them again. Hidden parts ARE exported: Fusion leaves hidden parts out of a
    STEP, so this script turns everything on before exporting (without saving the design).
  * mesh bodies (Fusion does not put them in STEP) as STL files in a "<name>.meshes" folder
  * a .f3d archive with the full Fusion history, as a safety copy
Folders mirror your Fusion projects and folders. Designs already exported and unchanged are
skipped, so it can be stopped and run again any time. If Fusion crashes on a design, the next
run skips that design and lists it at the end.
"""
import adsk.core
import adsk.fusion
import json
import os
import re
import time
import traceback

TITLE = 'Kestral: export all Fusion designs'
SCRIPT_VERSION = 3
# Kestral writes its own designs folder here when it saves this script.
KESTRAL_DEST = r'__KESTRAL_DEST__'


def _safe(name):
    name = re.sub(r'[<>:"/\\|?*\x00-\x1f]', '_', name or '').strip().rstrip('.')
    return name[:120] or 'untitled'


def _collect(folder, rel, out, progress):
    try:
        files = folder.dataFiles
        for i in range(files.count):
            if progress.wasCancelled:
                return
            df = files.item(i)
            try:
                if df.fileExtension.lower() == 'f3d':
                    out.append((df, rel))
            except Exception:
                pass
        subs = folder.dataFolders
        for i in range(subs.count):
            sub = subs.item(i)
            _collect(sub, rel + [_safe(sub.name)], out, progress)
    except Exception:
        pass
    adsk.doEvents()


def _box_mm(bb):
    """Fusion bounding box (cm) as [x0, y0, z0, x1, y1, z1] in millimeters."""
    a, b = bb.minPoint, bb.maxPoint
    return [round(v * 10.0, 4) for v in (a.x, a.y, a.z, b.x, b.y, b.z)]


def _body_boxes(root, comp, body):
    """Where a body sits in the whole design (one box per placement of its component). STEP files
    often lose body names (bodies in the top component have none), so Kestral finds hidden
    bodies by these boxes instead."""
    boxes = []
    try:
        if comp == root:
            boxes.append(_box_mm(body.boundingBox))
        else:
            occs = root.allOccurrencesByComponent(comp)
            for i in range(occs.count):
                try:
                    boxes.append(_box_mm(body.createForAssemblyContext(occs.item(i)).boundingBox))
                except Exception:
                    pass
    except Exception:
        pass
    return boxes


def _show_everything(design, log):
    """Turns on every hidden occurrence, body and bodies folder; returns what was hidden."""
    hidden = {'occurrences': [], 'bodies': [], 'bodyFolders': []}
    root = design.rootComponent
    try:
        occs = root.allOccurrences
        for i in range(occs.count):
            occ = occs.item(i)
            try:
                if not occ.isLightBulbOn:
                    hidden['occurrences'].append(occ.fullPathName)
                    occ.isLightBulbOn = True
            except Exception:
                log.write('     (could not show occurrence %s)\n' % occ.name)
    except Exception:
        pass
    comps = design.allComponents
    for i in range(comps.count):
        comp = comps.item(i)
        try:
            if not comp.isBodiesFolderLightBulbOn:
                hidden['bodyFolders'].append(comp.name)
                comp.isBodiesFolderLightBulbOn = True
        except Exception:
            pass
        try:
            bodies = comp.bRepBodies
            solid_count = bodies.count
            for j in range(bodies.count):
                b = bodies.item(j)
                try:
                    if not b.isLightBulbOn:
                        hidden['bodies'].append({'component': comp.name, 'body': b.name,
                                                 'only': solid_count == 1, 'root': comp == root,
                                                 'boxes': _body_boxes(root, comp, b)})
                        b.isLightBulbOn = True
                except Exception:
                    log.write('     (could not show body %s in %s)\n' % (b.name, comp.name))
        except Exception:
            pass
    return hidden


def _export_meshes(design, em, folder, stem, log):
    """Mesh bodies are not part of STEP: write each as STL in world position."""
    out = []
    root = design.rootComponent
    targets = []
    try:
        for j in range(root.meshBodies.count):
            targets.append(root.meshBodies.item(j))
        occs = root.allOccurrences
        for i in range(occs.count):
            occ = occs.item(i)
            mbs = occ.component.meshBodies
            for j in range(mbs.count):
                try:
                    targets.append(mbs.item(j).createForAssemblyContext(occ))
                except Exception:
                    targets.append(mbs.item(j))
    except Exception:
        return out
    if not targets:
        return out
    mdir = os.path.join(folder, stem + '.meshes')
    os.makedirs(mdir, exist_ok=True)
    used = set()
    for mb in targets:
        try:
            name = _safe(mb.name)
            base, n = name, 2
            while name.lower() in used:
                name = '%s (%d)' % (base, n)
                n += 1
            used.add(name.lower())
            path = os.path.join(mdir, name + '.stl')
            opts = em.createSTLExportOptions(mb, path)
            opts.sendToPrintUtility = False
            em.execute(opts)
            out.append(path)
        except Exception:
            log.write('     (mesh body %s could not be exported)\n' % getattr(mb, 'name', '?'))
    return out


def _had_hidden_bodies(step_path):
    """Version 3 only changed how hidden bodies are recorded: other designs stay as they are."""
    try:
        with open(os.path.splitext(step_path)[0] + '.kestral.json', encoding='utf-8') as f:
            return len(json.load(f).get('hidden', {}).get('bodies', [])) > 0
    except Exception:
        return True


def run(context):
    app = adsk.core.Application.get()
    ui = app.userInterface
    try:
        dest = KESTRAL_DEST if not KESTRAL_DEST.startswith('__') else os.path.join(
            os.path.expanduser('~'), 'Documents', 'Kestral Designs', 'From Fusion')
        answer = ui.messageBox(
            'This exports every design in every project you can see in Fusion, including hidden '
            'parts (they come back hidden in Kestral), mesh bodies and an .f3d backup of each design.\n\n'
            'Destination:\n' + dest + '\n\n'
            'Yes: use this folder\nNo: choose another folder\nCancel: stop\n\n'
            'Big libraries take a while. Designs already exported and unchanged are skipped, so you '
            'can press Stop and run this again later.',
            TITLE, adsk.core.MessageBoxButtonTypes.YesNoCancelButtonType,
            adsk.core.MessageBoxIconTypes.QuestionIconType)
        if answer == adsk.core.DialogResults.DialogCancel:
            return
        if answer == adsk.core.DialogResults.DialogNo:
            fd = ui.createFolderDialog()
            fd.title = 'Where should the designs go?'
            if fd.showDialog() != adsk.core.DialogResults.DialogOK:
                return
            dest = fd.folder
        os.makedirs(dest, exist_ok=True)
        backup_root = os.path.join(dest, '_Fusion backups (f3d)')
        state_path = os.path.join(dest, '.kestral_export_state.json')
        state = {}
        if os.path.exists(state_path):
            try:
                with open(state_path, 'r', encoding='utf-8') as f:
                    state = json.load(f)
            except Exception:
                state = {}
        designs = state.setdefault('designs', {}) if 'designs' in state else {
            k: v for k, v in state.items() if isinstance(v, dict) and 'path' in v}
        state = {'designs': designs, 'crashed': state.get('crashed', []), 'busy': state.get('busy')}
        # A design that was being exported when Fusion went down: skip it from now on.
        if state.get('busy'):
            if state['busy'] not in state['crashed']:
                state['crashed'].append(state['busy'])
            state['busy'] = None

        def save_state():
            with open(state_path, 'w', encoding='utf-8') as f:
                json.dump(state, f, indent=1)

        log = open(os.path.join(dest, 'export_log.txt'), 'a', encoding='utf-8')
        log.write('\n=== Export started %s (script v%d) ===\n' % (time.ctime(), SCRIPT_VERSION))

        progress = ui.createProgressDialog()
        progress.cancelButtonText = 'Stop'
        progress.isBackgroundTranslucent = False
        progress.isCancelButtonShown = True
        progress.show(TITLE, 'Finding your designs...', 0, 1, 0)

        jobs = []
        hubs = app.data.dataHubs
        multi_hub = hubs.count > 1
        for h in range(hubs.count):
            hub = hubs.item(h)
            projects = hub.dataProjects
            for p in range(projects.count):
                if progress.wasCancelled:
                    break
                proj = projects.item(p)
                rel = ([_safe(hub.name)] if multi_hub else []) + [_safe(proj.name)]
                progress.message = 'Finding designs in ' + proj.name + '...'
                adsk.doEvents()
                _collect(proj.rootFolder, rel, jobs, progress)

        total = len(jobs)
        progress.maximumValue = max(total, 1)
        claimed = set(v.get('path', '') for v in designs.values())
        done = skipped = failed = 0
        crashed_names = []
        for i, (df, rel) in enumerate(jobs):
            if progress.wasCancelled:
                break
            progress.progressValue = i
            progress.message = 'Design %d of %d: %s' % (i + 1, total, df.name)
            adsk.doEvents()
            try:
                key = df.id
                ver = df.versionNumber
            except Exception:
                key, ver = df.name + '/'.join(rel), -1
            if key in state['crashed']:
                crashed_names.append(df.name)
                continue
            entry = designs.get(key)
            if (entry and entry.get('version') == ver and os.path.exists(entry.get('path', ''))
                    and (entry.get('script', 1) >= SCRIPT_VERSION or
                         (entry.get('script', 1) == 2 and not _had_hidden_bodies(entry['path'])))):
                skipped += 1
                continue
            folder = os.path.join(dest, *rel)
            if entry and entry.get('path'):
                step_path = entry['path']
            else:
                step_path = os.path.join(folder, _safe(df.name) + '.step')
                n = 2
                while step_path in claimed or os.path.exists(step_path):
                    step_path = os.path.join(folder, '%s (%d).step' % (_safe(df.name), n))
                    n += 1
            os.makedirs(os.path.dirname(step_path), exist_ok=True)
            stem = os.path.splitext(os.path.basename(step_path))[0]
            state['busy'] = key
            save_state()
            doc = None
            try:
                try:
                    doc = app.documents.open(df, False)
                except Exception:
                    doc = app.documents.open(df)
                design = adsk.fusion.Design.cast(doc.products.itemByProductType('DesignProductType'))
                if not design:
                    raise Exception('not a 3D design')
                hidden = _show_everything(design, log)
                em = design.exportManager
                em.execute(em.createSTEPExportOptions(step_path))
                meshes = _export_meshes(design, em, os.path.dirname(step_path), stem, log)
                with open(os.path.join(os.path.dirname(step_path), stem + '.kestral.json'), 'w', encoding='utf-8') as f:
                    json.dump({'fusionName': df.name, 'version': ver, 'hidden': hidden,
                               'meshes': [os.path.basename(m) for m in meshes]}, f, indent=1)
                try:
                    bdir = os.path.join(backup_root, *rel)
                    os.makedirs(bdir, exist_ok=True)
                    em.execute(em.createFusionArchiveExportOptions(os.path.join(bdir, stem + '.f3d')))
                except Exception:
                    log.write('     (no .f3d backup for %s)\n' % df.name)
                designs[key] = {'version': ver, 'path': step_path, 'name': df.name, 'script': SCRIPT_VERSION}
                claimed.add(step_path)
                done += 1
                log.write('OK   %s -> %s\n' % (df.name, step_path))
            except Exception:
                failed += 1
                log.write('FAIL %s\n%s\n' % (df.name, traceback.format_exc()))
            finally:
                if doc:
                    try:
                        doc.close(False)   # never saves: the shown parts stay hidden in Fusion
                    except Exception:
                        pass
                state['busy'] = None
            if done % 3 == 0:
                save_state()
            adsk.doEvents()
        save_state()
        progress.hide()
        log.write('Exported %d, unchanged %d, failed %d, skipped after a crash %d\n' % (done, skipped, failed, len(crashed_names)))
        log.close()
        extra = ''
        if crashed_names:
            extra = ('\n\nSkipped because Fusion crashed on them before (export these by hand):\n  ' +
                     '\n  '.join(crashed_names[:15]) + ('\n  ...' if len(crashed_names) > 15 else ''))
        ui.messageBox(
            'Finished%s.\n\nExported: %d\nAlready up to date: %d\nFailed: %d%s%s\n\n'
            'Next, in Kestral: File > Move from Fusion > Import the exported library.\n\nFolder:\n%s'
            % (' (stopped early)' if progress.wasCancelled else '', done, skipped, failed,
               ' (see export_log.txt)' if failed else '', extra, dest), TITLE)
    except Exception:
        if ui:
            ui.messageBox('The export stopped:\n' + traceback.format_exc(), TITLE)
