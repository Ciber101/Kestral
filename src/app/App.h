#pragma once
#include "core/Document.h"
#include "core/Settings.h"
#include "jobs/JobSystem.h"
#include "kernel/Kernel.h"
#include "core/Json.h"
#include "print/Engine.h"
#include "print/BambuCloud.h"
#include "print/Orca.h"
#include "print/Preview.h"
#include "print/BedModel.h"
#include "print/Printer.h"
#include "render/Camera.h"
#include "render/Renderer.h"
#include "render/VkContext.h"
#include "app/SketchState.h"
#include "app/Thumbnails.h"
#include "app/Illustration.h"
#include "sketch/Solver.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/ViewCube.h"

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <future>
#include <set>
#include <thread>
#include <mutex>
#include <atomic>
#include <array>
#include <map>
#include "app/Keys.h"
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

struct GLFWwindow;

namespace kst {

enum class ToastKind { Info, Success, Warning, Error };

struct Tool {
    std::string id;
    std::string label;
    std::string shortcut;     // e.g. "E"
    ui::Icon icon = ui::Icon::None;
    int milestone = 0;        // 0 = available now, else the milestone that brings it
    std::function<void()> run;
    std::string tip;
    bool global = false;      // works the same in the model and in a sketch (undo, save, view...)
    std::string defShortcut;  // Fusion default; `shortcut` is the active binding
    bool available() const { return milestone == 0 && (bool)run; }
};

struct ToolGroup {
    std::string name;
    std::vector<int> tools;   // indices into App::tools_
    int primary = 2;          // how many big icons show on the toolbar
};

class App {
public:
    int run(int argc, char** argv);

    struct RecomputeResults;   // defined in SolidFeatures.cpp: worker output of a recompute pass
    struct AiCall { std::string id, name; std::string argsJson; };
    // A picture for the assistant (pasted, dropped or picked): PNG as base64, plus a preview texture.
    struct AiImage { std::string mime, b64; int w = 0, h = 0; ImTextureData* tex = nullptr; };
    struct AiTurn { std::string role; std::string text; std::vector<AiCall> calls; std::string callId, toolName;
                    std::string raw; std::string thinking; std::vector<std::shared_ptr<const AiImage>> images; };   // thinking: shown to the user only, never sent back   // raw: Anthropic content blocks (thinking must be sent back as is)
    struct AiLine { int kind; std::string text; double secs = 0; std::vector<std::shared_ptr<const AiImage>> images; bool folded = false; };   // 0 you, 1 assistant, 2 action, 3 error, 4 thinking, 5 picture helper description, 6 picture of the design

private:
    // ---- lifecycle (App.cpp)
    bool init(std::string& err);
    void shutdown();
    void frame();
    void registerTools();
    void emergencySave();

    // ---- document operations (App.cpp)
    void importDialog();
    void importPath(const std::string& path);
    void applyImport(kernel::ImportResult&& r, const std::string& path);
    void addImportToDoc(kernel::ImportResult& r, uint32_t featureId, const std::string& path);
    void pushUndo(const std::string& label);
    void undo();
    void redo();
    void newDocument();
    void deleteSelection();
    void toggleVisibility(const std::vector<uint32_t>& bodies);
    void setComponentVisible(uint32_t comp, bool vis);
    void isolate(const std::vector<uint32_t>& bodies);
    void exitIsolate();
    void showAll();
    void selectBodies(const std::vector<uint32_t>& ids, bool additive);
    void sendToSlicer();
    void exportStlDialog();
    std::vector<kernel::ExportItem> exportItems() const;
    std::string componentPath(uint32_t componentId) const;
    void setMarker(int pos);
    void toggleSuppress(int featureIndex);
    void deleteFeature(int featureIndex, bool undoStep = true);
    // Sketches and construction planes picked in the browser (bodies use selection_). Shift click picks a
    // range of rows, Ctrl click adds or removes one row. Delete removes everything picked at once.
    std::set<uint32_t> selSketches_, selPlanes_;
    int browserAnchor_ = -1;            // row index the last plain or Ctrl click landed on
    // After a delete: features that start failing because of it, so the user can undo right away.
    struct BreakCheck { bool pending = false; std::set<uint32_t> failedBefore; std::string what; std::vector<std::string> broken; bool show = false; };
    BreakCheck breakCheck_;
    void startBreakCheck(const std::string& what);
    void drawBreakCheck();
    void fitView(bool selectionOnly);
    bool bodyShown(const Body& b) const;
    void markSceneDirty() { sceneDirty_ = true; }
    void rebuildScene();
    void notYet(const Tool& t);
    void runTool(const std::string& id);
    int findTool(const std::string& id) const;

    // ---- solid features, recompute, files (SolidFeatures.cpp)
    void runExtrude();
    void runRevolve();
    void startSolidFeature(bool revolve);
    void editFeature(uint32_t featureId);
    void applyDialogValues();
    void drawFeatureDialog();
    void drawSectionDialog();
    void drawParamsDialog();
    void drawRecoveryPrompt();
    void drawStartPage();
    void sliceLoadBed();
    std::string versionsDir(const std::string& designPath) const;
    int latestVersion(const std::string& designPath) const;
    std::vector<int> designVersions(const std::string& designPath) const;
    void openDesignVersion(const std::string& designPath, int v);
    std::map<std::string, std::pair<long long, int>> versionCache_;   // data panel: path -> (mtime, version)                 // StartPage.cpp: New, Open, Recover, Recent with previews
    bool startOpen_ = false;
    bool themeDirty_ = false;
    void openParamsDialog();
    uint32_t pickSketchForFeature();
    void scheduleRecompute(int fromIndex);
    void pumpRecompute();
    void applyRecomputeResults();
    void refreshSignatures();
    void resolveParametricSketches();
    void syncBodyOverride(uint32_t bodyId);
    void saveDocumentNow();
    void saveDocumentAs();
    std::string designFolder() const;
    void openDocumentDialog();
    void openDocumentPath(const std::string& path);
    void checkRecovery();
    void updateAutosave();
    std::string autosavePath() const;

    // ---- UI (AppUI.cpp)
    void drawUI();
    void drawAppBar();
    void drawToolbar();
    bool toolButton(int toolIndex, float size, bool large);
    void toolMenuItems(const ToolGroup& g);
    void drawBrowser();
    void buildBrowserRows();
    void drawTimeline();
    void drawNavBar();
    void drawViewCube();
    void drawStatus();
    void drawToasts();
    void drawPalette();
    void drawSettings();
    void drawAppearance();
    void drawIsolateBanner();
    void handleViewportInput();
    void handleShortcuts();
    void toast(const std::string& msg, ToastKind kind = ToastKind::Info);

    // ---- sketch mode (SketchMode.cpp)
    sk::Sketch* curSketch();
    void beginCreateSketch(const std::string& pendingTool = {});
    void createSketchOn(sk::Plane plane, std::string tool);   // by value: callers pass sk_ members that get reset
    bool dimensionFor(const sk::Sketch& s, const std::vector<uint32_t>& picks, sk::Vec2 cursor, sk::Constraint& out) const;
    void editSketch(uint32_t sketchId);
    void finishSketch();
    void leaveSketchMode();
    void sketchToolCommand(const std::string& id);
    void setSketchTool(const std::string& id);
    void sketchUpdate();               // per frame: transforms, cursor, snapping, hover
    bool sketchMouseDown();            // left press inside the viewport, true if consumed
    void sketchMouseUp();
    void sketchMouseMove();
    void sketchClick();
    void sketchClickImpl();
    void sketchCommitSpline(bool closed, bool control = false);
    void sketchPointAt(const SnapInfo& si);
    void sketchCrossingClick();
    uint32_t addCrossingPoint(sk::Sketch& s, uint32_t curveA, uint32_t curveB, sk::Vec2 at);
    void sketchTextAt(const SnapInfo& si);
    void sketchTextUpdate();
    void retargetSketchProfiles(uint32_t sketchId);
    void sketchFilletAt(uint32_t hoverEnt, sk::Vec2 cursor, double radius = 0);
    void sketchChamferAt(uint32_t hoverEnt, sk::Vec2 cursor, double distance = 0);
    void sketchExtendAt(uint32_t ent, sk::Vec2 cursor);
    void sketchOffsetPick(uint32_t ent);
    double sketchOffsetDistance(sk::Vec2 cursor) const;
    bool sketchOffsetPreview(double d, std::vector<std::pair<sk::Vec2, sk::Vec2>>& lines,
                             std::vector<std::tuple<sk::Vec2, double, double, double>>& arcs) const;
    void sketchOffsetCommit(double d);
    void sketchProjectClick();
    int projectEdgesInto(sk::Sketch& s, uint32_t bodyId, const std::vector<uint32_t>& edges);
    void sketchIntersectClick();
    int intersectBodyInto(sk::Sketch& s, uint32_t bodyId);
    void sketchProjectFaceClick();
    // Drops the named curves onto the face along dir. An empty list uses every line, arc, and circle.
    // Returns the curves added, or -1 when the selection is not lines, arcs, or circles.
    int projectCurvesOnFace(sk::Sketch& s, uint32_t bodyId, int face, const std::vector<uint32_t>& ents, const glm::dvec3& dir);
    void sketchHud();
    void sketchCancel();
    void sketchDelete();
    void sketchToggleConstruction();
    void drawSketchOverlay();
    void drawSketchPalette();
    void drawDimensionEditor();
    bool sketchSolve(const sk::SolveOptions& o = {}, bool report = true);
    void sketchFinishOp();             // solve + refresh after an edit
    void sketchPlaneChoice();
    void applyConstraintTool();
    void dimensionTool();
    void trimAt(uint32_t ent, sk::Vec2 click);
    void breakAt(uint32_t ent);
    void mirrorSelection(uint32_t mirrorLine);
    // translate, rot, and scale say which part to apply. copy transforms duplicates and leaves the originals.
    bool sketchTransform(bool translate, bool rot, bool scale, bool copy, double dx, double dy, double angleDeg, double factor, sk::Vec2 pivot);
    // circular uses nx as the count and angleDeg as the total sweep. Otherwise nx, ny, sx, sy are the grid. Counts include the original.
    bool sketchPattern(bool circular, int nx, int ny, double sx, double sy, double angleDeg, sk::Vec2 pivot);
    void sketchSetDriven(uint32_t constraintId, bool driven);
    uint32_t sketchMakePoint(sk::Sketch& s, const SnapInfo& si);
    uint32_t pickEntityAt(const sk::Sketch& s, int mode, uint32_t exclude = 0) const;
    bool sketchKeys();
    void sketchLookAt();
    void sketchAfterHistory();
    bool sketchShown(const sk::Sketch& s) const;

    // ---- canvas interaction (Interaction.cpp)
    using SubRef = std::pair<uint32_t, uint32_t>;   // (body, face / edge index) or (sketch, profile index)
    enum PickFilter : uint32_t { PickBody = 1, PickFace = 2, PickEdge = 4, PickProfile = 8, PickVertex = 16 };
    enum class HoverKind { None, Body, Face, Edge, Profile, Vertex };
    struct HoverInfo {
        HoverKind kind = HoverKind::None;
        uint32_t body = 0, index = 0;     // body (or sketch for profiles) and face / edge / profile index
        bool hasPoint = false;
        glm::dvec3 point{0};
    };
    struct EdgeCacheEntry { ShapePtr shape; std::vector<std::vector<glm::dvec3>> polys; bool ready = false; };
    struct ProfCache { uint64_t rev = 0; std::vector<sk::Profile> profiles; };
    struct MarkMenu {
        bool open = false, clickMode = false, sub = false;
        ImVec2 center{0, 0}, origin{0, 0};
        double t0 = 0;
    };
    struct Manip {
        bool active = false, dragging = false;
        int target = 0;                   // 1 extrude distance, 2 tool value (press pull, offset)
        glm::dvec3 origin{0}, dir{0, 0, 1};
        double value = 0, grab = 0;
        double minValue = -1e300;         // fillet / chamfer: keep the radius positive
    };
    std::set<SubRef> selFaces_, selEdges_, selProfiles_;
    HoverInfo hov_;
    std::map<uint32_t, ProfCache> profCache_;
    std::map<const ShapeHandle*, EdgeCacheEntry> edgeCache_;
    uint32_t activePickFilter_ = PickBody | PickFace | PickEdge | PickProfile;
    bool tangentChain_ = false;      // off: a click picks just that edge (turn on in the dialog for the chain)
    bool winSel_ = false;
    ImVec2 winA_{0, 0};
    MarkMenu mm_;
    Manip manip_;
    bool manipHovered_ = false;
    glm::dvec3 orbitPivot_{0};
    bool orbitPivotValid_ = false;
    int savedMarker_ = -1;            // marker position to restore after editing a feature / sketch

    glm::dmat4 viewProj() const;
    bool worldToScreen(const glm::dvec3& w, ImVec2& out) const;
    bool rayAt(ImVec2 px, glm::dvec3& o, glm::dvec3& d) const;
    bool hitPoint(uint32_t bodyId, uint32_t face1, ImVec2 px, glm::dvec3& out) const;
    const std::vector<std::vector<glm::dvec3>>* bodyEdges(uint32_t bodyId);
    const std::vector<sk::Profile>& sketchProfiles(const sk::Sketch& s);
    void clearSelection();
    bool anySelection() const;
    std::vector<uint32_t> selectedBodies() const;
    uint32_t pickFilterNow() const;
    void updateHover();
    uint32_t pickBodyFor(uint32_t hovered) const;
    void selectionChanged();
    void viewportClick(bool add, bool dbl);
    void windowSelect(ImVec2 a, ImVec2 b, bool add);
    void drawInteractionOverlay();
    void setManip(int target, const glm::dvec3& origin, const glm::dvec3& dir, double value);
    void drawManipulator();
    void manipCommit(double value, bool final);
    void openMarkingMenu(ImVec2 at);
    void drawMarkingMenu();
    void commandOk();
    void commandCancel();
    void lookAtFace(uint32_t bodyId, uint32_t face);
    void createSketchOnFace(uint32_t bodyId, uint32_t face);
    bool createSketchOnFace(uint32_t bodyId, uint32_t face, std::string tool);
    void syncToolFromSelection();
    void syncExtrudeFromSelection();
    void closeFeatureDialog(bool ok);
    void closeToolDialog(bool ok);
    void restoreMarkerAfterEdit();
    void beginOrbit(ImVec2 m);
    void measureClick();
    ImVec2 selPressPos_{0, 0};
    bool selPressDouble_ = false;

    // ---- sketch state
    SketchState sk_;
    std::vector<ToolGroup> sketchGroups_;
    // Keyboard shortcuts
    std::vector<KeyCombo> toolCombo_;
    std::vector<bool> toolIsSketch_;
    bool keysOpen_ = false;
    int keyRec_ = -1;                   // tool whose shortcut is being recorded
    int tbCtxTool_ = -1;
    uint32_t revealBody_ = 0;           // browser: expand to and scroll to this body's row
    int revealTries_ = 0;
    // ---- AI assistant (Assistant.cpp)
    struct AiState {
        bool open = false, settings = false, busy = false, scroll = false, focus = false;
        std::vector<AiLine> lines;
        std::vector<AiTurn> history;
        std::string input;
        std::string keyEdit;
        std::shared_ptr<std::atomic<bool>> cancel;
        std::vector<std::string> models;   // from "Get list"
        bool fetching = false;
        std::string fetchMsg;
        // The chosen model's real context window (asked from the service when it can tell).
        int ctxMax = 0;
        bool ctxReported = false, ctxFetching = false;
        std::string ctxFor;                 // provider|model the numbers above belong to
        std::set<std::string> visionModels; // installed models that can see pictures (Ollama tells)
        int mainVision = -1;                // the chosen model sees pictures (-1 not known yet)
        std::string visionFor;              // provider|model mainVision belongs to
    } ai_;
    std::vector<std::shared_ptr<const AiImage>> aiAttach_;   // pictures waiting in the input box
    std::vector<ImTextureData*> aiImageTex_;                  // their previews, freed with the chat
    bool aiAttachImage(const uint8_t* rgba, int w, int h);
    // A picture made off the main thread (look_at_design): the data now, its preview texture later on the main thread.
    struct AiImageBuilt { std::shared_ptr<AiImage> img; std::vector<uint8_t> small; int tw = 0, th = 0; };
    static AiImageBuilt aiBuildImage(const uint8_t* rgba, int w, int h);
    void aiImageTexture(AiImageBuilt& b);
    // The design as items for a picture (bodies shown, or just the named ones), and which way the view looks.
    struct AiShotScene { std::vector<thumb::Item> items; glm::vec3 camFwd{0, 1, 0}, camUp{0, 0, 1}; std::string missing; };
    AiShotScene aiShotScene(const std::vector<std::string>& bodies) const;
    bool aiAttachFile(const std::string& path);
    bool aiPasteImage();
    void aiClearChat();
    static bool isImageFile(const std::string& path);
    // The model's thinking and answer while they stream in (worker writes, UI reads).
    std::mutex aiLiveMx_;
    std::string aiLiveThink_, aiLiveText_;
    double aiLiveT0_ = 0;
    bool aiLiveStarted_ = false;       // the first streamed line arrived
    // Messages typed while the assistant works: it reads them before its next step (steering).
    std::mutex aiSteerMx_;
    struct AiSteer { std::string text; std::vector<std::shared_ptr<const AiImage>> images; };
    std::vector<AiSteer> aiSteer_;   // typed while it works (with any pictures)
    // While waiting for a local Ollama model: what Ollama says about it (loading, GPU / CPU split).
    std::string aiLiveUrl_, aiLiveModel_, aiLiveOllama_;
    bool aiLiveIsOllama_ = false;
    std::atomic<bool> aiPsBusy_{false};
    double aiPsAt_ = -1e9;
    bool aiLiveOpen_ = false;          // the live Thinking dropdown is expanded
    std::string aiLiveStage_;          // what the assistant is doing now ("Loading the model", "Running extrude"...)
    int aiLastPromptTok_ = 0, aiLastOutTok_ = 0, aiCtxWindow_ = 0;   // last round trip: tokens in / out, window used
    double aiLastOutRate_ = 0, aiLastInRate_ = 0;                    // tokens per second (writing / reading)
    int aiPromptToolCount_ = 0;        // tools sent with the last request
    std::set<std::string> aiActiveTools_;   // smart tools: loaded so far in this chat
    std::string aiGpuDismissed_;       // the graphics card warning the user said to ignore (this session)
    struct AiChoice {
        bool open = false;
        std::string title, text;
        std::vector<std::string> options;
        std::shared_ptr<std::promise<int>> answer;
    } aiChoice_;
    std::string aiSystemPromptLean() const;
    std::set<std::string> aiPickTools(const std::string& userText) const;
    std::string aiFindTools(const std::string& query, std::set<std::string>& active) const;
    std::string aiContextNote() const;
    void drawAssistantPrefs();
    int prefSection_ = 0;
    // The search box at the top right (GlobalSearch.cpp) and the blink that shows where a result is.
    struct SearchHit { int kind; std::string label, path, key; int a = 0, b = 0; int score = 0; };   // kind: 0 command, 1 slice tool, 2 slice page, 3 workspace, 4 setting, 5 preset, 6 preference
    struct GlobalSearchState {
        std::string text, last;
        std::vector<SearchHit> hits;
        int sel = 0;
        bool open = false, focus = false, scrollTo = false;
    } gsearch_;
    std::vector<SearchHit> globalSearch(const std::string& query);
    void globalSearchGo(const SearchHit& h);
    void drawGlobalSearch(ImDrawList* dl, float right, float y, float h);
    std::string flashKey_;
    double flashT0_ = -10;
    bool flashScroll_ = false, flashOpenGroup_ = false;
    float flashAmount(const std::string& key);
    void flashRect(ImDrawList* dl, ImVec2 a, ImVec2 b, float amount, float rounding);
    void flashItem(const std::string& key);
    void flashStart(const std::string& key);
    bool slicePrewarmed_ = false;
    // ---- Illustration (IllustrationUI.cpp): pictures of the design for instructions and posts
    struct IllusUi {
        bool open = false, busy = false, dirty = true, saving = false, selectedOnly = false;
        illus::Options opt;
        int size = 0;               // index into the size list
        ImTextureData* tex = nullptr;
        int texW = 0, texH = 0, numbered = 0;
        uint64_t sig = 0;
        std::string status;
    } illus_;
    std::vector<illus::Part> illusParts(bool selectedOnly, const std::vector<std::string>& names) const;
    illus::Options illusOptions(int w, int h) const;
    void illusPreview();
    void illusSave(bool svg);
    void drawIllustration();
    std::string illusTest_;   // developer check: save here once the design is built
    bool gsearchTest_ = false, gsearchTestGo_ = false;
    bool aiTestSend_ = false;
    bool aiFollow_ = true;          // the chat keeps the newest message in view (off while you read further up)
    float aiLastScrollY_ = 1e9f;
    int aiSeenLines_ = 0;           // messages that were in view when you scrolled up
    std::string prefFlash_;            // a setting to flash after a search jump
    double prefFlashT0_ = -10;
    bool aiLiveFollow_ = true;         // keep the newest thinking line in view
    void aiFetchModels();
    void aiFetchModelInfo();
    // Messages Kestral shows while an assistant tool runs ("Nothing was picked"...) go back to the
    // model as part of the tool's answer, so it knows what happened.
    bool aiCapture_ = false;
    std::vector<std::string> aiCaptured_;
    bool aiOpenedCommand_ = false;      // a command the assistant opened is waiting for picks
    void aiLog(const std::string& who, const std::string& text);   // ai_logs/<date>.txt
    void drawAssistant();
    void aiSend(const std::string& text, bool showLine = true, std::vector<std::shared_ptr<const AiImage>> pics = {});
    std::string aiSystemPrompt() const;
    std::string aiRunTool(const std::string& name, const std::string& argsJson);
    // ---- Slicing and printing (PrintAssistant.cpp). These tools run on the assistant's worker
    // thread (slicing and printer talk take seconds to minutes); `post` runs code on the main thread.
    using PostFn = std::function<void(std::function<void()>)>;
    static bool aiIsPrintTool(const std::string& name);
    std::string aiPrintTool(const std::string& name, const std::string& argsJson, const PostFn& post,
                            const std::shared_ptr<std::atomic<bool>>& cancel);
    // ---- Shared by the assistant and the MCP server (AssistantShared.cpp). Safe off the main thread.
    bool designPicture(const std::string& argsJson, const PostFn& post, const std::shared_ptr<std::atomic<bool>>& cancel,
                       AiImageBuilt& built, std::string& head, std::string& order, std::string& err);
    // The printer's camera, the newest picture (starts the camera when it is off). Off the main thread.
    bool cameraPicture(const PostFn& post, const std::shared_ptr<std::atomic<bool>>& cancel, AiImageBuilt& built, std::string& msg);
    std::string illustrationTool(const std::string& argsJson, const PostFn& post, const std::shared_ptr<std::atomic<bool>>& cancel,
                                 AiImageBuilt* outImg);
    // ---- MCP server (McpServer.cpp): other AI apps use the assistant's tools. See app/Mcp.h.
    struct McpServer;
    struct McpCall { std::string text; std::vector<std::shared_ptr<const AiImage>> images; bool error = false; };
    struct McpStatus { bool running = false; int port = 0; std::string error, client, tool; int calls = 0; double idleSecs = -1; };
    void mcpStart();
    void mcpStop();
    void mcpRestart();
    void mcpShutdown();
    McpStatus mcpStatus() const;
    void drawMcpPrefs();
    void mcpServeConnection(std::shared_ptr<McpServer> srv, intptr_t sock);
    McpCall mcpRunTool(const std::string& client, const std::string& name, const std::string& args,
                       const std::shared_ptr<std::atomic<bool>>& cancel);
    json::Value mcpToolList() const;            // Assistant.cpp: every tool, MCP style
    static bool mcpKnownTool(const std::string& name);
    static bool mcpReadOnlyTool(const std::string& name);   // safe for the read-only key
    void mcpAudit(const std::string& client, const std::string& tool, const std::string& args, const McpCall& r, double ms, bool readOnly);
    bool hiddenWindow_ = false;   // --hidden: no window on screen (batch jobs, a server)
    std::shared_ptr<McpServer> mcp_;
    // MCP's control of the whole window (McpUi.cpp): screenshots, mouse and keyboard, camera and more.
    json::Value mcpUiToolList() const;
    static bool mcpIsUiTool(const std::string& name);
    McpCall mcpRunUiTool(const std::string& client, const std::string& name, const json::Value& args,
                         const std::shared_ptr<std::atomic<bool>>& cancel);
    struct McpInput { int kind = 0; float x = 0, y = 0; int button = 0; bool down = false; int key = 0; std::string text; float wheel = 0; double until = 0; int frames = 0; };
    std::deque<McpInput> mcpInput_;     // main thread: fed to Dear ImGui a frame at a time
    float mcpCurX_ = -1, mcpCurY_ = -1;  // MCP's mouse, window pixels
    double mcpCursorT_ = -1e9, mcpHoldUntil_ = -1e9;
    std::atomic<float> mcpShotScale_{1.f};   // window pixels per pixel of the last screenshot
    void mcpFeedInput();
    void mcpDrawCursor();
    struct PrintState {
        std::mutex m;                       // guards setup / last (worker and main thread)
        print::OrcaSetup setup;
        double setupAt = -1e9;
        print::SliceResult last;
        std::string lastName, lastChanges;
        // "Print this?" card in the assistant panel; the answer goes to the waiting tool.
        bool askOpen = false;
        std::string askTitle, askText, askYes = "Print";
        std::shared_ptr<std::promise<bool>> askAnswer;
        // Preferences > Printer
        bool testing = false, setupLoading = false;
        std::string testMsg;
        std::string exeHint;                // OrcaSlicer program, for worker threads
    } print_;
    print::PrinterConn printerConn() const;
    print::OrcaSetup orcaSetupCached(bool refresh);
    void drawPrintConfirm();
    void drawPrinterPrefs();
    void notifyAssistantDone(const std::string& reply);   // notification + sound (Preferences)
    // Bambu Lab cloud printers (through Bambu's network plugin, print/BambuCloud.*)
    struct BambuCloudUi {
        bool loading = false, checked = false;
        print::BambuCloudStatus status;
    } bambu_;
    void bambuCloudRefresh();
    void drawBambuCloudPicker();
    void openInOrca(const std::string& file);
    std::vector<uint32_t> aiFindBodies(const std::vector<std::string>& names, std::string& missing) const;
    std::string aiBodyPath(uint32_t bodyId) const;
    // Modeling tools beyond the first set (AssistantModel.cpp). False means some other tool handles it.
    bool aiModelTool(const std::string& name, const std::string& argsJson, std::string& out);
    void aiNameOutput(uint32_t featureId, const std::string& name, uint32_t folder);
    uint32_t aiEnsureFolder(const std::string& name);
    void modelTestStep();
    void uiTestStep();
    bool uiTest_ = false;
    bool modelTest_ = false;
    std::string toolScript_, toolScriptOut_;   // --run-tools <script> <design.kst>
    void toolScriptStep();
    // Robot export (Robot.cpp)
    struct RobotModel;
    RobotModel buildRobotModel() const;
    std::string robotSummary(const RobotModel& m) const;
    std::string exportRobot(const std::string& urdfPath, std::string& report);
    std::string robotTool(const std::string& argsJson);
    void drawRobotWindow();
    std::string compPath(uint32_t cid) const;
    uint32_t compByPath(const std::string& path) const;
    bool robotOpen_ = false;
    std::string robotCheck_;
    double robotCheckT_ = -1, robotEditT_ = 0;
    std::vector<std::string> robotLinks_;
    int robotSel_ = -1;
    bool robotLabels_ = true;
    std::string robotMassFor_;
    void drawRobotOverlay();
    void moveOldDesigns();
    void rememberComponentName(uint32_t cid);
    bool modelTestFail_ = false;
    void stampActiveComponent(uint32_t featureId);
    void activateComponent(uint32_t id);
    void groundComponent(uint32_t id, bool on);
    struct Triad {
        bool dragging = false;
        int axis = -1;
        glm::dvec3 center{0};
        double startVal = 0, grab = 0;
    } triad_;
    void drawMoveTriad();
    // Hang watchdog: a second thread that notices when the main loop stops (a freeze), saves a
    // recovery copy of the last known good state and offers to close Kestral cleanly.
    std::thread watchdog_;
    std::atomic<bool> wdStop_{false};
    std::atomic<int64_t> wdBeatMs_{0};
    std::atomic<const char*> wdPhase_{"starting"};
    std::mutex wdSnapMu_;
    std::string wdSnapshot_;          // last serialized document (refreshed every few seconds when dirty)
    double wdSnapTime_ = -1;
    void startWatchdog();
    void stopWatchdog();
    void watchdogBeat(const char* phase);
public:
    void modalFrame();                // a frame from inside Windows' move / resize loop
private:
    bool inFrame_ = false;
    bool autosaveBusy_ = false;
    double lastAutosaveTime_ = -1;
    std::string typeAhead_;             // digits typed in the viewport during a command
    int typeAheadFrames_ = 0;
    char keyFilter_[64] = {};
    std::vector<uint32_t> clipboard_;   // Ctrl+C bodies
    void applyKeyBindings();
    void setToolShortcut(int ti, const std::string& combo);
    void beginKeyEdit(int ti);
    void drawKeysDialog();
    int toolForCombo(const KeyCombo& c) const;
    void runShortcut(int ti);
    void copySelection();
    void pasteClipboard();
    void selectAll();   // construction planes hidden from the browser eye

    // ---- core state
    GLFWwindow* window_ = nullptr;
    VkContext vk_;
    Renderer renderer_;
    Camera cam_;
    Document doc_;
    UndoStack undo_;
    Settings settings_;
    std::unique_ptr<JobSystem> jobs_;
    ui::Fonts fonts_;
    ui::ViewCube viewCube_;
    float uiScale_ = 1.f;
    double lastPresentT_ = 0;   // frame cap (main loop)
    double lastScaleCheck_ = -1;
    float autoUiScale() const;
    void applyUiScale(float s);
    void keepWindowsOnScreen();
    std::string settingsPath_;
    bool dirty_ = false;     // unsaved changes

    // ---- scene / interaction
    bool sceneDirty_ = true;
    uint64_t sceneRev_ = 0;
    Aabb sceneBounds_;
    std::set<uint32_t> selection_;
    std::set<uint32_t> browserHover_;      // bodies highlighted from the browser
    std::set<uint32_t> isolated_;          // non empty = isolate mode
    uint32_t hoverBody_ = 0, hoverFace_ = 0;
    bool viewportHovered_ = false;
    enum class NavMode { None, Orbit, Pan, Zoom } navMode_ = NavMode::None;
    enum class Drag { None, Orbit, Pan, Zoom, Select } drag_ = Drag::None;
    float dragDist_ = 0.f;
    double lastMiddleClick_ = -10.0;
    int dragBtn_ = -1;              // mouse button driving the current view drag
    ImVec2 dragStart_{0, 0};
    int navFor(int button) const;   // Drag kind for a button + held modifiers in the chosen style

    // ---- UI state
    std::vector<Tool> tools_;
    std::vector<ToolGroup> solidGroups_, utilGroups_;
    int activeTab_ = 0;
    bool browserOpen_ = true;
    bool showViewCube_ = true, showNavBar_ = true;
    float browserWidth_ = 270.f;
    char browserSearch_[128] = {};
    int browserFilter_ = 0;     // 0 all, 1 bodies, 2 components, 3 hidden, 4 selected
    std::set<uint32_t> collapsed_;   // collapsed component ids (and folder ids)
    uint32_t renameId_ = 0;
    char renameBuf_[256] = {};
    bool renameIsComp_ = false;
    struct Row { int kind; uint32_t id; int depth; std::string label; int count; };   // kind 0 doc,1 folder,2 comp,3 body,4 origin,5 sketches folder,6 sketch
    std::vector<Row> rows_;
    uint64_t rowsRev_ = 0;
    std::string rowsKey_;
    bool paletteOpen_ = false;
    char paletteBuf_[128] = {};
    int paletteSel_ = 0;
    bool settingsOpen_ = false;
    bool appearanceOpen_ = false;
    bool appearanceUndoPushed_ = false;
    int matCategory_ = 0;              // appearance library tab
    char matSearch_[64] = {};
    int matApplyFaces_ = 0;            // 0 apply to bodies, 1 to faces
    bool matKeepFileColors_ = false;   // keep an imported file's face colors when coloring a body
    int matDrag_ = -1;                 // library swatch being dragged onto the model
    bool matDragOverView_ = false;
    void applyLook(const glm::vec3& color, const MatProps& m, bool toFaces,
                   const std::vector<uint32_t>& bodies, const std::set<std::pair<uint32_t, uint32_t>>& faces);
    bool pendingSendAfterSetup_ = false;
    int timelineHover_ = -1;
    bool draggingMarker_ = false;
    std::string lastToolId_;
    bool playing_ = false;
    double playT_ = 0.0;
    Row ctxRow_{-1, 0, 0, {}, 0};
    bool confirmNew_ = false;
    bool donateSnoozed_ = false;       // "not now" for this session
    bool closeAsk_ = false, quitting_ = false;
    void drawCloseAsk();
    // ---- Slice workspace (SliceTab.cpp): Kestral's own slicer screen. OrcaSlicer's engine slices
    // hidden in the background; nothing of OrcaSlicer is ever on screen.
    bool sliceWs_ = false;
    std::string orcaExe_;
    bool restartAfterExit_ = false;
    bool sliceTestPending_ = false;   // KESTRAL_SLICE_TEST   // "Restart Kestral" after a slicer engine update
    struct SliceTab {
        // What gets sliced with what (empty: OrcaSlicer's current choice)
        std::string machine, process, filament, bedType;
        std::map<std::string, std::string> overrides;   // Orca setting key -> value, on top of the profiles
        bool onlySelected = false;
        bool arrange = true, orient = false;            // Prepare toolbar (applied when slicing)
        // Orca style sidebar and windows
        int page = 0;                                    // process page (Quality, Strength...)
        int top = 1;                                     // Orca's top tabs: 0 Home 1 Prepare 2 Preview 3 Device 4 Project 5 Calibration
        bool objectsView = false;                        // Process: Global (false) or Objects
        std::string objSearch;                           // Objects: add a setting (search)
        int viewType = 0;                                // Preview colors: 0 line type 1 filament 2 speed 3 height 4 width 5 fan 6 temperature 7 flow
        bool gcodeView = true;                           // Preview: the G-code lines panel
        int thumbPlate = -1;
        int paintMode = 0;                               // Prepare painting: 1 color 2 support on 3 support off 4 seam
        int paintFil = 1;
        bool measure = false;                            // Prepare: measure two points
        std::vector<glm::vec3> measurePts;
        bool cutMode = false;                            // Prepare: cut the selected part
        float cutZ = 10.f;
        int cutKeep = 0;                                 // 0 both, 1 upper, 2 lower
        bool varLayer = false;                           // variable layer height window
        int calPage = 0;                                 // Calibration page
        int devPage = 0;                                 // Device: 0 status 1 storage 2 update 3 HMS
        bool sendAms = true;                             // Send print: use AMS
        bool showLabels = false, showOverhang = false;   // view menu
        std::vector<int> sliceQueue;                     // Slice all: plates still to slice
        bool advanced = true;                            // OrcaSlicer's Advanced switch (on: every setting)
        int editWin = 0;                                 // 1 printer settings, 2 filament settings
        int editPage = 0;
        std::string gcodeKey, gcodeBuf;                  // G-code script being edited
        // Preview extras
        int moveHi = -1;                                 // lines shown in the top layer (-1: all)
        bool sendOpen = false;                           // "Send print" dialog
        // The plate (Prepare): every visible body is an object; its copies sit at pos[i].
        struct PlateObj {
            std::vector<glm::vec2> pos;          // plate position (center) of each copy, mm
            float rotZ = 0;                      // degrees around the plate's up axis
            glm::mat4 lay{1.f};                  // lay flat rotation (applied before rotZ)
            glm::vec3 scale{1.f};
            bool onPlate = true;
            std::vector<int> plateOf;            // build plate of each copy (missing: plate 0)
            // The part itself: it carries its own geometry, so a tab keeps working (and can hold
            // parts of several designs) whatever design is open.
            std::string src;                     // design it came from (sliceDocKey of that design)
            uint64_t stable = 0;                 // (feature, slot) of the body in that design
            uint32_t bodyId = 0;                 // the body now, while that design is open (else 0)
            std::string name;
            MeshPtr mesh;
            ShapePtr shape;
            glm::mat4 bodyXform{1.f};
            glm::vec3 color{0.72f, 0.74f, 0.77f};
            bool useColor = true;                // (old) print it in its own color; now: fil below
            int fil = 0;                         // filament it prints with (0 based into the tab's fils)
            bool autoFil = true;                 // Kestral picks the filament from the part's color
            std::map<std::string, std::string> settings;   // per object settings (Orca: Objects > Add settings)
            uint32_t group = 0;                  // split to parts: parts of one object share a group (moves together)
            MeshPtr paintMesh;                   // the mesh with painted faces colored (display)
            uint64_t paintKey = ~0ull;
            std::map<uint32_t, int> paintColor;  // painted faces: face -> filament (1 based)
            std::map<uint32_t, int> paintSupport;  // face -> 1 enforce, 2 block
            std::map<uint32_t, int> paintSeam;   // face -> 1 enforce, 2 block
            // exact bounds of the rotated / scaled part (before moving it to pos), cached
            bool cacheValid = false;
            uint64_t cacheRev = 0;
            Aabb core;
        };
        std::map<uint32_t, PlateObj> plate;
        // What a tab keeps besides the plate (swapped as one with the parked tabs).
        struct Fil {
            std::string preset;                  // filament profile ("" = filament 1's)
            glm::vec3 color{1.f, 0.42f, 0.21f};
            bool autoMade = false;               // made by Kestral for a part's color (goes when unused)
        };
        struct Extra {
            std::vector<Fil> fils;               // Orca's filament list: filament 1 is the tab's main filament
            std::vector<int> slicedFils;         // tab filament of each filament in the last slice
            std::vector<std::string> plateNames; // build plate names (Orca: the name above the plate)
            std::vector<char> plateLocked;       // locked plates are left alone by Arrange
            std::vector<std::string> plateBeds;  // plate type per plate ("" = the tab's)
            std::vector<int> amsMap;             // AMS tray for each sliced filament (ams*4+slot, 254 external, -1 not set)
            std::vector<float> flush;            // flushing volumes, N x N (mm3), N = fils.size()
            float flushMul = 1.f;
            bool flushAuto = true;               // false once the user typed a value
            std::string projName, projAuthor, projDesc, projLicense;   // Project page
            std::vector<print::SliceResult> plateResults;       // the last slice of each plate (thumbnails, Slice all)
            std::map<std::string, std::string> tabOverrides;    // settings for this tab only (calibration tests)
            std::map<std::string, PlateObj> gone;               // parts whose body went away (hidden, undone): their place, if they return
            int calKind = 0;                                    // a calibration tab: 1 flow ratio, 2 temperature, 3 pressure advance
            std::vector<float> calValues;                       // the value each test piece / block prints with
        } ex;
        // When not empty, the plate holds only these bodies (set from the design tab's selection
        // when the Slice tab is entered; cleared to show every visible body again).
        std::set<uint32_t> plateOnly;
        int plates = 1, activePlate = 0;                 // build plates (Orca's Add plate); the one sliced
        uint32_t nextObj = 1;                            // plate object ids (tab local)
        std::set<std::pair<uint32_t, int>> plateSelSet;  // selected parts (object, copy), plateSel is the main one
        int gizmo = 0;                                   // 1 move (M), 2 rotate (R), 3 scale (S)
        int gizmoDrag = 0;                               // 1 X arrow, 2 Y arrow, 3 free / ring / scale handle
        glm::vec2 gizmoStart{0.f}, gizmoCenter{0.f};
        float gizmoAngle0 = 0, gizmoScale0 = 1;
        std::map<std::pair<uint32_t, int>, glm::vec2> gizmoPos0;   // positions at drag start
        std::map<uint32_t, float> gizmoRot0;
        std::map<uint32_t, glm::vec3> gizmoScl0;
        bool boxSel = false;
        bool ctxOpen = false;                            // open the part menu this frame
        std::vector<std::pair<uint32_t, int>> clip;      // Ctrl+C
        ImVec2 boxA{0, 0};
        bool multiColor = true;                          // parts print in their own colors
        bool live = true;                                // the tab's design is the open one (else a snapshot)
        uint64_t plateRev = 0;                           // design revision the plate was synced to
        uint32_t plateSel = 0;                           // selected body (0: none)
        int plateSelCopy = 0;
        bool layMode = false;                            // next click on a face lays the part on it
        std::vector<std::pair<uint32_t, int>> pickMap;   // render id - 1000 -> (body, copy)
        bool plateDrag = false;
        glm::vec2 dragOffset{0.f};
        bool plateOpen = true;                           // the Objects section is unfolded
        bool sectPrinter = true, sectFilament = true;
        // Device tab (the printer, like OrcaSlicer's Device tab)
        bool device = false;
        print::PrintStatus devStatus;
        bool devPolling = false, devBusy = false, devAskStop = false;
        double devLastPoll = -1e9;
        std::string devMsg;
        bool sendLeveling = true, sendFlowCali = false, sendTimelapse = false;
        bool choicesLoaded = false, setupLoading = false;
        std::shared_ptr<const print::OrcaSetup> setup;   // profiles (read on a worker)
        std::vector<const print::Preset*> printers, filaments, processes;   // into *setup
        std::string choicesFor;                          // machine the filament / process lists fit
        std::string editKey, editBuf;                    // setting being typed in
        Camera designCam;                                // the design view, back when leaving the tab
        bool haveDesignCam = false;
        // Settings lists (full settings of the chosen profiles, read on a worker)
        std::string settingsFor;                         // machine|process|filament they belong to
        json::Value processJson, filamentJson, machineJson;
        json::Value allJson;                             // every setting with the engine's defaults
        bool settingsLoading = false;
        std::string search;
        // Slicing job
        bool slicing = false;
        double sliceStart = 0;
        std::shared_ptr<std::atomic<bool>> cancel;
        std::string error;
        uint64_t slicedRev = 0;          // design revision that was sliced
        // Preview
        std::shared_ptr<const print::Toolpaths> paths;
        std::vector<std::array<MeshPtr, print::kFeatureCount>> meshes;   // per layer, per feature
        std::vector<std::array<MeshPtr, 16>> meshesTool;   // per layer, per filament (prints with several)
        std::vector<glm::vec3> toolColors;                 // filament colors of the last slice
        bool colorByFilament = true;                       // (old) preview colors: filaments
        std::vector<std::array<MeshPtr, 16>> meshesScalar; // per layer, per gradient step (speed, height... views)
        int scalarMode = -1, scalarBuilding = -1;
        glm::vec3 nozzleAt{0.f};                           // the move slider's nozzle
        bool nozzleShown = false;
        bool syncFromPrinter = false, syncAmsFils = false;  // apply the printer's nozzle / AMS once its status comes
        bool fil1ColorSet = false;                         // filament 1's color was picked (not the profile's)
        bool savePresetOpen = false, compareOpen = false, searchOpen = false, objAddOpen = false;
        std::string savePresetName;
        bool uniformScale = true;
        bool legendFolded = false;
        uint64_t devSeq = 0, devSeqSeen = 0;            // printer status updates
        std::string tempEdit, tempBuf;
        int calPick = 0;
        float legendBottom = 0;
        std::string pathsFile;           // print file the preview shows
        bool loadingPreview = false;
        int layerLo = 0, layerHi = 0;
        std::array<bool, print::kFeatureCount> show{};
        bool preview = false;            // the view shows the toolpaths (false: the design)
        MeshPtr bed;
        print::BedScene bedScene;        // the printer's real bed: model, grid, plate labels
        std::string bedFor;
        bool bedLoading = false;
        bool framed = false;
        int dragSlider = 0;              // 1 top handle, 2 bottom handle
        // Slicer engine (Kestral's own copy of OrcaSlicer's engine)
        print::Engine engine;
        std::string engineVersion;       // what the engine in use reports
        bool engineVersionAsked = false;
        print::EngineRelease latest;
        bool checking = false, checked = false, installing = false;
        double installFrac = -1;
        std::string installStage, installError, pending;
        std::shared_ptr<std::atomic<bool>> engineCancel;
        std::mutex m;                    // installStage / installFrac (worker writes)
    } slice_;
    std::string orcaPath();
    std::string orcaPathFind();
    void enterSliceWorkspace(int tab = -1);
    void leaveSliceWorkspace();
    // ---- slice tabs (SliceTabs.cpp): one per design; the active one's state lives in slice_.
    struct SliceDocState {
        std::string key, name;
        std::set<std::string> members;   // other designs combined into this tab (their keys)
        std::map<uint32_t, SliceTab::PlateObj> plate;
        uint32_t nextObj = 1;
        std::set<std::pair<uint32_t, int>> plateSelSet;
        std::set<uint32_t> plateOnly;
        int plates = 1, activePlate = 0;
        uint64_t plateRev = 0;
        uint32_t plateSel = 0;
        int plateSelCopy = 0;
        std::shared_ptr<const print::Toolpaths> paths;
        std::vector<std::array<MeshPtr, print::kFeatureCount>> meshes;
        std::vector<std::array<MeshPtr, 16>> meshesTool;
        std::vector<glm::vec3> toolColors;
        SliceTab::Extra ex;
        std::string pathsFile;
        int layerLo = 0, layerHi = 0, moveHi = -1;
        bool preview = false;
        uint64_t slicedRev = 0;
        std::string error;
        print::SliceResult last;
        std::string lastName, lastChanges;
        std::string pendingPreview;                      // sliced while another tab was in front
        struct Snap { MeshPtr mesh; glm::mat4 xform{1.f}; uint32_t rgba = 0xFFC0C0C0u; int plate = 0; };
        std::vector<Snap> snap;                          // the plate as it was, for a design not open now
        bool live = true;
    };
    std::vector<SliceDocState> sliceDocs_;
    int sliceActive_ = -1;
    int sliceTabDrag_ = -1;   // slice tab being dragged onto another (combine)
    // ---- design tabs (DesignTabs.cpp)
    struct DesignTab {
        Document doc;
        UndoStack undo;
        bool dirty = false;
        Camera cam;
        std::set<uint32_t> selection, isolated, collapsed;
        std::set<std::pair<uint32_t, uint32_t>> selFaces, selEdges, selProfiles;
        std::vector<uint32_t> selOrder;
        int savedMarker = -1;
    };
    std::vector<DesignTab> designTabs_ = std::vector<DesignTab>(1);   // [designActive_] is a placeholder
    int designActive_ = 0;
    int closeAskTab_ = -1;
    uint64_t docGen_ = 1;   // changes when another design comes in front (late rebuild results are dropped)
    void designSwap(DesignTab& t);
    bool docInUse() const;
    void switchDesignTab(int i);
    int newDesignTab();
    void closeDesignTab(int i);
    int designTabFor(const std::string& path) const;
    std::string designTabTitle(int i) const;
    std::string sliceDocKey() const;
    int sliceTabFor(const std::string& key) const;
    bool sliceTabOwns(int tab, const std::string& key) const;   // the design's parts belong to this tab
    bool plateOwns(const std::string& key) const;                 // ... to the tab in front
    void sliceSwap(SliceDocState& d);
    void sliceSnapshot();
    void sliceActivate(int i);
    void enterSliceTab(int i);
    void closeSliceTab(int i);
    glm::vec2 plateOffset(int plate) const;
    int plateAt(glm::vec2 world) const;
    int plateOfCopy(const SliceTab::PlateObj& o, int k) const { return k >= 0 && k < (int)o.plateOf.size() ? o.plateOf[(size_t)k] : 0; }
    void drawSliceBar();
    void drawSliceWorkspace();
    void drawSlicePanel();
    void drawSliceLayerSlider();
    void drawSliceMoveSlider();
    void drawSliceLegend();
    void drawSliceSettingsWindow();
    void drawSliceSendDialog();
    void drawSlicePrepareToolbar();
    void drawSliceDevice();
    // Plate (Plate.cpp)
    float sliceTop() const { return (34.f + 40.f) * uiScale_; }   // below the Slice tab row (Orca's tabs)
    float sliceSideW() const { return 400.f * uiScale_; }         // the Slice tab's side panel
    void setSliceTop(int tab);   // Orca's top tabs: 0 Home 1 Prepare 2 Preview 3 Device 4 Project 5 Calibration
    int sliceCurTop() const;
    bool slicePrepareShown() const { return sliceCurTop() == 1; }
    void drawSlice3DToolbar();
    void drawSlicePlateOverlays();
    void drawSliceViewButtons();
    void drawSliceToolPanels();      // cut, paint, measure, variable layer windows
    void drawSliceHome();
    void drawSliceProject();
    void drawSliceCalibration();
    void drawFlushDialog();
    bool flushOpen_ = false;
    uint32_t plateAddPart(const std::string& src, MeshPtr mesh, ShapePtr shape, const glm::mat4& bodyXform, const std::string& name, glm::vec3 color);
    void plateAddFromFile();
    void plateAutoOrient(uint32_t oid);
    void plateOrientPlate(int plate);
    void plateSplit(uint32_t oid, bool parts);
    void plateCutSelected();
    void plateDeletePlate(int plate);
    void plateToFront(int plate);
    void plateNewPlate(bool makeActive = true);
    std::string plateName(int plate) const;
    void sliceLookFrom(glm::vec3 dir);
    void sliceAllPlates();
    void sliceSaveGcode();
    void plateSync();
    bool plateInFilter(uint32_t objId) const;
    std::string plateTabKey() const;
    bool plateIsSel(uint32_t oid, int copy) const;
    void plateSelect(uint32_t oid, int copy, bool add);
    std::vector<std::pair<uint32_t, int>> plateSelection() const;
    glm::vec2 plateSelCenter();
    void plateDeleteSelection();
    void plateAddCopies(int n);
    bool plateGizmoGeom(ImVec2& c, float& r, ImVec2& ax, ImVec2& ay);
    void drawPlateGizmo();
    void drawPlateContextMenu();
    void sliceMergeTabs(int from, int into);
    glm::vec3 sliceFilamentColor(int slot) const;
    // Filaments of the tab (SliceFils.cpp)
    bool isPlainColor(glm::vec3 c) const;           // the default body grey: prints with filament 1
    glm::vec3 bodyPrintColor(const Body& b) const;
    std::string sliceFilPreset(int i) const;
    glm::vec3 sliceFilColor(int i) const;
    glm::vec3 presetColor(const std::string& filamentPreset);
    std::map<std::string, glm::vec3> presetColors_;
    void sliceFilsEnsure();
    int sliceAddFil(const std::string& preset, glm::vec3 color, bool autoMade);
    int sliceNewFilColor() const;
    glm::vec3 slicePaletteColor(int k) const;
    void sliceRemoveFil(int i);
    int sliceFilForColor(glm::vec3 c);
    void sliceAutoFils();
    void sliceFlushDefaults();
    bool sketchInUse(uint32_t sketchId) const;
    glm::mat4 plateXform(uint32_t bodyId, int copy);
    const Aabb& plateCore(uint32_t bodyId);
    Aabb plateBedRect() const;
    void plateArrange();
    void plateArrangeOne(int plate);
    void platePack(std::vector<std::pair<uint32_t, int>> items, std::vector<int> plates);
    void platePlaceNew(const std::vector<std::pair<uint32_t, int>>& items);
    void plateLayFlat(uint32_t bodyId, int face);   // face -1: its biggest flat face
    bool plateInstanceOnBed(uint32_t bodyId, int copy);
    void drawSlicePlatePanel(float width);
    bool handlePlateInput(bool hovered);
    void devicePoll(bool now);
    void deviceCommand(const std::string& action);   // pause / resume / stop
    void deviceSend(const std::string& gcodeOrJson, bool json, const std::string& what);   // G-code or a raw MQTT command
    void deviceLight(bool on);
    void deviceSpeed(int level);
    void sliceAutoAmsMap();
    void sliceStartCalibration(int kind, int pass);
    MeshPtr plateBoxMesh(float w, float d, float h);
    MeshPtr platePaintMesh(SliceTab::PlateObj& o);
    bool plateRayHit(uint32_t oid, int copy, ImVec2 px, glm::vec3& hitOut);
    // One OrcaSlicer setting row (label, the right control, unit, revert); tab: process / filament / printer.
    void drawOrcaOption(const std::string& key, const std::string& line = std::string());
    void drawOrcaPage(const std::string& tab, const std::string& page, const std::string& search);
    std::string sliceValue(const std::string& key) const;
    std::string sliceBaseValue(const std::string& key) const;
    void sliceSetValue(const std::string& key, const std::string& value);
    void handleSliceViewInput();
    void sliceLoadChoices();
    void sliceLoadSettings();
    void startSliceJob();
    void showSliceResult(const print::SliceResult& r);   // main thread: tab shows it, preview loads
    void loadSlicePreview(const std::string& gcode3mf);
    void sliceBuildScalarMeshes(int mode);
    bool slicePresetCombo(const char* id, std::string& cur, const std::vector<const print::Preset*>& list, const char* none, float width, bool frameless);
    void drawSliceObjects(float w, float height);
    void drawSavePresetDialog();
    std::string sliceSaveProcessPreset(const std::string& name);
    std::string sliceWarningText(const std::string& w) const;
    void sliceAfterStatus();          // printer status arrived: nozzle, AMS filaments
    void drawSlicePlateThumbs();
    void drawSliceGcodeView();
    void drawSliceNoPreview();
    print::PrinterCamera camera_;
    ImTextureData* camTex_ = nullptr;
    uint64_t camSeq_ = 0;
    int camW_ = 0, camH_ = 0;
    bool camOn_ = true;
    std::map<std::string, ImTextureData*> plateThumbTex_;   // sliced file -> its plate picture
    ImTextureData* makeTexture(const uint8_t* rgba, int w, int h);
    void dropTexture(ImTextureData* t);
    ImTextureData* sliceThumbFor(const std::string& gcode3mf);
    void rebuildSliceScene();
    void slicePrint();
    void sliceSaveFile();
    void engineCheck(bool install);   // looks for a newer engine; install: also downloads it
    void restartKestral();
    std::string engineStatusText();
    // ---- Move from Fusion (FusionMove.cpp)
    struct LibEntry {
        std::string step, kst, rel;    // exported STEP, Kestral design next to it, path shown
        int status = 0;                // 0 new, 1 updated since imported, 2 already imported
        bool pick = false;
    };
    bool fusionMoveOpen_ = false;
    std::string fusionScriptDir_;
    std::string libFolder_;
    std::vector<LibEntry> libEntries_;
    bool libScanning_ = false, libRunning_ = false, libShowDone_ = false, libFinished_ = false;
    int libDone_ = 0, libTotal_ = 0, libMade_ = 0, libFailed_ = 0, libHidden_ = 0;
    std::string libFailMsg_;
    CancelPtr libCancel_;                 // Stop: also interrupts the geometry kernel where it can
    int libMadeLive_ = 0;
    void stopFusionImport();
    void drawFusionMove();
    void writeFusionScript();
    void scanFusionLibrary(const std::string& folder);
    void importFusionLibrary(std::vector<LibEntry> picks);
    struct LibPiece { std::shared_ptr<kernel::ImportResult> r; std::string src; };
    bool saveImportAsDesign(std::vector<LibPiece>& pieces, const std::string& sidecar, const std::string& out,
                            std::string& err, int& hiddenApplied);
    // ---- Data panel: every design in Kestral Designs, Fusion style (DataPanel.cpp)
    bool dataOpen_ = false;
    std::string dataCur_;                 // folder shown
    double dataScanT_ = -10;
    struct DataItem { std::string name, path; bool dir; long long mtime; int count = 0; int version = 0; };
    bool dataGrid_ = true;   // data panel: preview cards (true) or a compact list
    std::vector<DataItem> dataItems_;
    char dataSearch_[96] = {};
    std::string dataSel_;
    void drawDataPanel();
    void openDataPanel(const std::string& folder);
    // ---- Design previews (Thumbnails.cpp, DataPanel.cpp)
    struct ThumbTex { ImTextureData* tex = nullptr; long long stamp = 0; bool missing = false; };
    std::unordered_map<std::string, ThumbTex> thumbs_;   // design path -> preview
    std::vector<ImTextureData*> thumbGraveyard_;
    std::set<std::string> thumbTried_;                  // made from the STEP this session (or failed)
    bool thumbBusy_ = false;
    int thumbLoadsThisFrame_ = 0;
    std::string thumbAfterRebuild_;                      // design waiting for its geometry
    ImTextureData* thumbFor(const std::string& designPath);
    void thumbFromItems(const std::string& designPath, std::vector<kernel::ExportItem> items);   // renders on a worker
    void thumbFromDoc(const std::string& designPath);   // the open design's visible bodies
    void updateThumbnails();
    void flushTextureGraveyard();
    void drawDonateNote();
    void drawDonateBanner();
    bool renameIsSketch_ = false;

    struct Toast { std::string msg; ToastKind kind; double t0; };
    std::deque<Toast> toasts_;

    // ---- solid tools M4 (SolidTools.cpp)
    void runFillet();
    void runChamfer();
    void runShell();
    void runThicken();
    void runPressPull();
    void runRectPattern();
    void runCircPattern();
    void runPathPattern();
    void runMirror();
    void runCombine();
    void runSplitBody();
    void runSplitFace();
    void runHole();
    void runSweep();
    void runLoft();
    void runMove();
    void runScale();
    void runReplaceFace();
    void runBox();
    void runCylinder();
    void runSphere();
    void runCone();
    void runTorus();
    void runWedge();
    void runTube();
    void makeSolidPrimitive(bool cylinder);
    sk::Sketch* openPrimitiveSketch(const sk::Plane& pl, const char* fail);
    bool extrudePrimitive(uint32_t sid, int which, const char* dist, const char* fail);
    bool revolvePrimitive(uint32_t sid, const char* fail);
    void closeNamedPrimitive(const char* stem, const char* suffix);
    void runAlign();
    void runOffsetPlane();
    void runAnglePlane();
    void runMidPlane();
    void runThreePointPlane();
    void runTangentPlane();
    void startConstructPlane(int kind);
    void runAxis();
    void runPoint();
    void updateConstructAxis(const Feature& ft);
    void updateConstructPoint(const Feature& ft);
    void runDraft();
    void runCoil();
    void runRib();
    void runWeb();
    void runEmboss();
    void runBoss();
    void runSnap();
    void runRest();
    void runLip();
    void runSilhouette();
    void runNewComponent();
    void runGround();
    void runJoint();
    void runAsBuilt();
    void runRigid();
    void runPipe();
    void runMeasure();
    void runSection();
    std::string applySection(bool on);
    void runInterference();
    void convertToSolid(uint32_t bodyId);
    void updateConstructPlane(const Feature& ft);
    void drawMeasurePanel();
    void drawConstructPlanes();
    void drawOriginPlanes();
    void exportStepDialog();
    void export3mfDialog();
    std::vector<std::pair<uint32_t, std::string>> sketchesBeforeMarker() const;
    void startToolFeature(FeatureType type);
    void fillToolDlgFromParams(const Feature& f);
    void editToolFeature(Feature& f);
    void applyToolDialog();
    void drawToolDialog();
    void runRemove();
    // Gears and threads (GearThreadTools.cpp).
    void runGear();
    void runThread();
    void drawGearDialog(bool& ok);
    void drawThreadDialog(bool& ok);
    void syncThreadFromSelection();
    // Wing generator (WingTools.cpp).
    void runWing();
    void drawWingDialog(bool& ok);
    // Airfoil combo + .dat import + typed name (after a label). True when it changed.
    bool drawAirfoilPicker(std::string& name, float indent);
    // Dovetail generator (DovetailTools.cpp).
    void runDovetail();
    void drawDovetailDialog(bool& ok);
    // Blade generator (BladeTools.cpp).
    void runBlade();
    void drawBladeDialog(bool& ok);
    void drawCosmeticThreads();
    TargetRef refOfBody(uint32_t bodyId) const;
    std::vector<std::pair<uint32_t, uint32_t>> selectionRefs() const;   // selection as (feature, slot)

    // ---- solid feature dialog state
    struct FeatureDlg {
        bool open = false;
        uint32_t featureId = 0;
        bool isRevolve = false;
        char dist[64] = {};
        char taper[64] = {};
        char angle[64] = {};
        char thin[64] = {};           // blank is a solid
        int thinSide = 0;             // 0 center, 1 one side, 2 the other side
        int op = 0;
        int axis = 0;                 // 0 Y, 1 X, 2+ line entity (index into axisLines)
        std::vector<std::pair<uint32_t, std::string>> axisLines;
        bool flip = false, symmetric = false;
        std::string err;
        double distVal = 0;           // evaluated, for display
        bool opAuto = true;           // operation follows the drag direction until the user picks one
        bool inFront = false, inBack = false;   // a body lies in front of / behind the profile
        int extent = 0;               // 0 distance, 1 up to a face
        FaceRef to;                   // the face an extrude stops on
        std::string toName;
        bool focus = false;           // focus the main value field on the next draw (type-to-focus)
        bool editing = false;         // editing an existing feature (not creating)
        uint32_t hiddenSketch = 0;    // sketch to auto hide on OK (Fusion habit)
    };
    FeatureDlg dlg_;
    bool focusFirstField_ = false;
    bool paramsOpen_ = false;
    std::string paramsErr_;
    std::string paramsUndoKey_;       // coalesces undo while typing in one field
    int newParamSel_ = -1;

    // ---- M4 tool dialog and edge / face picking
    struct ToolDlg {
        bool open = false;
        uint32_t featureId = 0;
        FeatureType type = FeatureType::Other;
        char v1[64] = {}, v2[64] = {}, v3[64] = {}, v4[64] = {}, v5[64] = {}, v6[64] = {};   // expression fields
        char v7[64] = {}, v8[64] = {}, v9[64] = {};                                          // snap, slider, three point plane
        int option = 0, option2 = 0, option3 = 0;                  // plane / axis / op / path orientation
        bool flag = false;                                          // path pattern: reverse the path
        bool splitManual = false;                                   // split: the user chose the tool mode themselves
        bool pickWait = false;                                      // path pattern: the next click is the path
        glm::dvec3 axisO{0}, axisD{0, 0, 1};                        // circular pattern: picked edge axis
        std::string axisName;
        TargetRef pathBody{0, 0};                                   // path pattern: edge's body
        uint32_t pathEdge = 0;
        std::vector<TargetRef> targets;
        std::vector<std::pair<uint32_t, uint32_t>> edges, faces;    // (target index, edge/face index)
        uint32_t edgeTotal = 0, faceTotal = 0;
        double v1Val = 0;
        std::vector<uint32_t> sketchIds;                            // sweep / loft sketch list
        std::vector<uint32_t> railIds;                              // loft guide sketches
        std::vector<std::string> sketchNames;
        int selA = 0, selB = 1;                                     // sweep profile / path indices
        std::vector<TargetRef> tools;                               // combine tool bodies
        GearParams gear;                                            // gear / thread values being edited
        ThreadParams thread;
        WingParams wing;
        DovetailParams dovetail;
        BladeParams blade;
        AlignParams align;
        bool editing = false;
        bool focus = false;
    };
    ToolDlg tdlg_;
    // Hole tool (H): click faces to place holes, snapping to the grid, corners, edge middles and
    // circle centers (HoleTool.cpp).
    struct HolePlace {
        bool active = false;
        bool snapGrid = true, snapEdges = true;
        float grid = 1.f;               // mm
        bool valid = false;             // a spot under the cursor
        glm::dvec3 pos{0}, dir{0, 0, -1};
        std::string snapName;           // what it snapped to
    } holePlace_;
    bool holeMode() const;
    void startHolePlacement();
    void updateHolePlacement();
    void holePlaceClick();
    void drawHolePlacement(ImDrawList* dl);
    void drawHoleDialogPlacement();
    // Adds placed holes as a Hole feature (assistant).
    std::string addPlacedHoles(const std::vector<std::pair<glm::dvec3, glm::dvec3>>& pts, double dia, double depth, int kind,
                               double extraDia, double extra, const std::vector<TargetRef>& only = {},
                               double spotDia = 0, double spotDepth = 0, double drillAngle = 0, bool tapped = false, double pitch = 0);
    std::vector<uint32_t> selOrder_;                                         // bodies in the order they were picked
    uint32_t lastSelected_ = 0;                                              // combine's tool body
    bool measureMode_ = false;
    int measureKind_ = 0;             // 0 distance, 1 angle, 2 radius, 3 area, 4 loop
    bool sectionOpen_ = false;
    int sectionAxis_ = 2;
    bool sectionFlip_ = false;
    char sectionOff_[64] = "0";
    struct MeasurePick { uint32_t body = 0; uint32_t face = 0; uint32_t edge = 0; std::string name; };   // face, edge: +1 encoded
    std::vector<MeasurePick> measurePicks_;
    double measureDist_ = -1;
    bool measureBusy_ = false;

    // ---- recompute state
    int recomputeQueuedFrom_ = -1;
    bool recomputeRunning_ = false;
    std::shared_ptr<RecomputeResults> recomputeResults_;
    double lastSaveActivity_ = 0.0;
    std::string recoveryFile_;

    // ---- timing
    double time_ = 0.0;
    float fps_ = 0.f;
    int redrawFrames_ = 3;
    std::chrono::steady_clock::time_point lastFrame_;
public:
    void activity() { redrawFrames_ = 4; }
    bool allowClose();                // window X: false (and asks to save) when there are unsaved changes
    void requestQuit();               // File > Exit: same question
    void dropFiles(int count, const char** paths);
};

} // namespace kst
