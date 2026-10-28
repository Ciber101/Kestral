# Kestral

Fast parametric CAD for Windows and Linux, with a Fusion 360 style workflow, a built in slicer and
printer control, and an AI assistant that can do anything you can do in the program.

Kestral is free and open source (GNU AGPL v3). It is made by one person and is still young: expect
rough edges, and please report them.

## What it does
* **Design**: sketches with constraints, extrude, revolve, sweep, loft, fillets, chamfers, shells,
  holes, patterns, mirror, booleans, components and joints, a parametric timeline and user
  parameters. Generators for gears, threads, coils, pipes, wings, blades, dovetails and more.
  STEP, STL and OBJ in; STEP and STL out. Moves every design out of Fusion 360 in one go.
* **Slice**: a Slice tab with OrcaSlicer's engine inside Kestral. Plates, arranging, painting
  (supports, seams, colors), every OrcaSlicer setting, layer preview, calibration prints, and your
  own OrcaSlicer printer, filament and process profiles.
* **Print**: send to Bambu Lab printers (cloud or LAN), OctoPrint, Klipper (Moonraker) and
  PrusaLink. Printer status, camera, AMS, temperatures and controls on the Device page.
* **Assistant** (Ctrl+Space): tell it what to make or change in plain words. It builds, edits,
  slices and prints with Kestral's own tools, checks its work by looking at the model, and models
  things from pictures you paste in. Works with local models through Ollama or LM Studio (free, on
  your own computer) and with Claude, OpenAI, Gemini, Grok, DeepSeek, Mistral, OpenRouter, Groq and
  any OpenAI compatible service (your own API key).
* **Search** (Ctrl+F): finds any command, slicer setting, preset or preference, takes you there and
  points it out.

## Download (Windows)
Get the latest zip from [Releases](https://github.com/Ciber101/Kestral/releases), unzip it anywhere
and run `Kestral.exe`. It needs a graphics driver with Vulkan 1.3 (any recent NVIDIA, AMD or Intel
driver). The Slice tab uses OrcaSlicer's engine: an installed OrcaSlicer, or a copy Kestral
downloads for you from the Slice tab. Your OrcaSlicer profiles show up in Kestral.

## Build it yourself

### Windows
1. **Visual Studio 2022 Build Tools** (free): https://visualstudio.microsoft.com/downloads/
   (scroll to "Tools for Visual Studio", "Build Tools for Visual Studio 2022").
   In the installer tick **Desktop development with C++**. That includes CMake and Ninja.
2. **Git for Windows**: https://git-scm.com/download/win
3. Double click `build.bat` (or run it in a terminal to watch progress). The first build downloads
   vcpkg and compiles OpenCASCADE, GLFW, ImGui and the rest into `C:\KestrelBuild` (set
   `KESTRAL_BUILD` for another folder). Expect 20 to 40 minutes once; after that a rebuild takes
   seconds. The program is copied to `bin\`.
4. Double click `run.bat`, or drag a STEP/STL/OBJ/Kestral file onto `bin\Kestral.exe`.

### Linux (any distro)
```
bash build.sh     # asks before installing system packages (apt, dnf, pacman or zypper)
bash run.sh       # or open Kestral from your app menu
```
* Needs a GPU driver with Vulkan 1.3 (Mesa 22+ or the NVIDIA driver). Works on X11 and Wayland.
* File dialogs use zenity (GNOME, XFCE, others) or kdialog (KDE). Pasting pictures into the
  assistant uses wl-paste or xclip.
* Designs are saved in the `designs` folder inside the Kestral folder.

## The assistant
Open it with Ctrl+Space and pick a provider with the gear button. For a free, private setup install
[Ollama](https://ollama.com) and pull a model that supports tools (for example a Qwen 3 model).
A model that cannot see pictures can still work from screenshots and photos: pick a vision model
as the picture helper in Preferences > Assistant, and it describes them for the main model.
Kestral only starts a print after you press **Print**, unless you allow it in the settings.

## Controls (Fusion style)
| Action | Input |
|---|---|
| Orbit | Shift + middle drag: orbits around exactly the point you pressed on (green dot), or drag the view cube |
| Set orbit center | Shift + middle click on the model: that point becomes the view center |
| Pan | Middle drag |
| Zoom | Wheel (zooms at the cursor) |
| Fit / Home | F6 or double click middle / Home |
| Select | Click a face, edge, body or sketch profile. Ctrl or Shift + click adds or removes |
| Window select | Drag left to right = fully inside, right to left = crossing |
| Tangent chain / whole body | Double click an edge / a face |
| Marking menu | Right click (click, or flick in a direction) |
| OK / Cancel a command | Enter / Esc (Ctrl+Z inside a command also cancels) |
| Type a value | During any command just type the number, it goes into the value box. Letters stay shortcuts |
| Delete | Delete or Backspace. A body adds a Remove step; faces are deleted and the body heals (removes fillets, holes) |
| Command search | S |
| Extrude / Hole / Press pull / Fillet / Chamfer | E / H / Q / F / Shift+C |
| Move / Joint / Measure / Appearance | M / J / I / A |
| Sketch: line, rectangle, circle, trim, offset, project | L, R, C, T, O, P (L, R, C or P outside a sketch starts one: pick the plane, then draw) |
| Sketch: dimension, construction | D, X |
| Sketch: typed values | While drawing just type a number, Tab to the next field, Enter to place |
| Finish sketch | E or H go straight to Extrude or Hole, or the green Finish Sketch |
| Compute all | Ctrl+B |
| Browser / view cube / nav bar | Ctrl+Alt+B / Ctrl+Alt+V / Ctrl+Alt+N |
| Hide/show selection | V |
| Copy / Paste bodies | Ctrl+C / Ctrl+V (paste opens Move with the copy and its X/Y/Z arrows) |
| Select all | Ctrl+A (in a sketch: all sketch curves) |
| New / Save / Save As / Open | Ctrl+N / Ctrl+S / Ctrl+Shift+S / Ctrl+O |
| Insert file | Ctrl+I, or drag and drop |
| Send to slicer | Ctrl+P |
| Undo / Redo | Ctrl+Z / Ctrl+Y |
| Solo in browser | Alt + click the eye |
| Change any shortcut | Right click a toolbar command, or Preferences > Keyboard Shortcuts |

Selection works like Fusion: pick the faces, edges or profiles first, then the command (or the
other way round). Extrude a face to push or pull it; the operation (Join, Cut, New Body) picks
itself from the direction, and a drag arrow sits on the model for the distance. Editing a feature
from the timeline rolls the model back to it, like Fusion.

### Modeling tools
* **Fillet (F)**: hover the body, click edges (orange = selected, yellow = hover), type a radius.
  Never crashes: if a radius does not fit, Kestral fillets what it can and tells you the maximum
  radius for each edge that failed. Esc stops picking; the preview updates on every value change.
* **Chamfer**: same edge picking with a distance.
* **Shell**: pick the faces to remove (or none for a closed cavity), set wall thickness.
* **Press Pull (Q)**: hover a flat face and press Q, set the offset, or click more faces.
* **Rectangular / Circular Pattern**: select bodies first. Circular: click a round edge (hole, boss) or a straight edge to spin around it. Copies are instances: 500 track
  links cost the same as one, for drawing and for the timeline.
* **Mirror**: mirrored copies across XY / XZ / YZ.
* **Combine**: target body plus tool bodies, Join, Cut or Intersect, with Keep Tools.
* **Split Body**: cut selected bodies with a plane into separate bodies.
* **Hole (H)**: with nothing selected, click faces to place holes. Snaps to the grid, corners, edge
  middles, circle centers and the face center. Simple, counterbore, countersink, spotface, drill point
  and tapped (ISO metric). Circles in a selected sketch work too. H again or Enter finishes.
* **Generators (CREATE menu)**: Gear (spur, helical, herringbone, internal, rack, bevel, worm, worm
  wheel), Thread (modeled or cosmetic; ISO metric coarse and fine, UNC/UNF, BSP, NPT, ACME,
  trapezoidal, buttress and more), Coil, Pipe, Wing (NACA or .dat airfoils), Blades (propeller,
  rotor, ducted fan, wind turbine, boat), Dovetail joints, Rib, Web, Emboss, Boss, Snap Fit, Lip.
* **Assemble and construct**: components, rigid / revolute / slider joints, as-built joints, rigid
  groups, offset, angle, mid, 3 point and tangent planes, axes, points.
* **The assistant** can do all of this too, from plain words (Ctrl+Space opens it).
* All of the above accept expressions everywhere a number is typed, and all rebuild
  parametrically when the underlying sketch, body or parameter changes.

### Modeling
* **Extrude (E)**: with a sketch done (or the latest sketch picked), opens a Fusion style dialog.
  Distance takes plain numbers or expressions (`width / 2`). Operation: New Body, Join, Cut,
  Intersect (targets = whatever you have selected). Symmetric, flip and taper angle included.
  Every change previews live in the viewport; OK keeps it, Cancel undoes it.
* **Revolve**: same dialog with an axis picker (Y, X, or any line in the sketch) and angle.
* **Parametric timeline**: edit a sketch and Finish Sketch, drag the marker, suppress or delete
  features: Kestral recomputes only what changed, on a worker thread. The viewport never waits.
  The timeline tooltip shows each feature's recompute time. Failed features turn red with the
  reason, and the model keeps everything else.
* **User parameters** (MODIFY > Change Parameters): add named values, use them in any sketch
  dimension or extrude distance. Change a parameter and the sketches and solids update.
* **Files**: native `.kst` designs (Ctrl+S), autosave every 30 seconds (settable in Preferences),
  and crash recovery: after a crash or forced close, Kestral offers the autosaved work on start.
  Imports (STEP/STL/OBJ) are stored as file references and re-read on open.

Sketch tools: lines, rectangles (2 point, center, 3 point), circles, arcs, ellipses, polygons, four kinds of slot,
fit point and control point splines, text (with any installed font), points. MODIFY: Fillet, Chamfer, Trim,
Break, Extend, Offset, Mirror, Move / Copy, Pattern, Project, Intersect, Onto Face. Constraints and driven dimensions.

## Files
* Settings and log: `%APPDATA%\Kestral` (Linux: `~/.config/kestral`)
* Autosaves: `%APPDATA%\Kestral\autosave`
* Slicer engine: `%APPDATA%\Kestral\engine` (Linux: `~/.local/share/kestral`)

## Tests
* `bin\Kestral.exe --model-test` runs about 230 geometry checks through the assistant tools and closes
  itself. Results: `kestral.log` and `%TEMP%\kestral-model-test.txt`. `KESTRAL_TEST_FROM=<step>` starts
  at one step.
* `bin\Kestral.exe --ui-test` opens and cancels every toolbar command. Results:
  `%TEMP%\kestral-ui-test.txt`.

## If something breaks
Kestral itself never closes on an error: it shows a message, logs it to
`%APPDATA%\Kestral\kestral.log`, and keeps going. Please open an issue with what you did and the end
of that log. If a build fails, run `build.bat` from a terminal and include the first error.


## Support
Kestral is free. If it saves you time, you can buy the creator a coffee at
[ko-fi.com/ciberai](https://ko-fi.com/ciberai).

## License
Kestral is free software under the [GNU Affero General Public License v3 or later](LICENSE).
The libraries it uses and their licenses are listed in [THIRD_PARTY.md](THIRD_PARTY.md).
