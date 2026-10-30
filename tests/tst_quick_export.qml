import QtQuick
import QtQuick.Controls.Basic
import QtTest
TestCase {
    id: test
    name: "QuickExportState"
    when: windowShown
    width: 1000; height: 800
    QtObject {
        id: service
        signal changed()
        signal submissionFinished(string planId, string jobId)
        property bool busy: false
        property bool importing: false
        property string error: ""
        property var plan: ({})
        property var job: ({})
        property var jobs: []
        property var modelSources: []
        property var inputInfo: []
        property var localInputRoots: ["C:/"]
        property var sourcePaths: []
        property var sourceRoots: []
        property var importResult: ({})
        property var capabilities: ({write_targets:["png"],model_export:{input_formats:["psk","psa"]}})
        property int submissions: 0
        property string cancelledId: ""
        function nameExamples() { return {names:["source-converted.png"]} }
        function invalidatePlan() { plan = {}; changed() }
        function preflight() { busy = true; plan = {}; changed() }
        function submit(ack) { submissions++; busy = true; job = {}; changed() }
        function cancelPreparation() { cancelledId = "preparation"; changed() }
        function cancel(kind,id) { cancelledId = id; changed() }
        function jobSnapshot(id) { return JSON.parse(JSON.stringify(jobs.find(j => j.id === id) || {})) }
        function selectJob(id) { job = jobSnapshot(id); changed() }
        function pauseJob() {}
        function removeJob() {}
    }
    QtObject {
        id: testController
        property string language: "en"
        property var imageJobs: service
        function styleToolWindow(w) {}
        function localFile(p) { return p }
        function copy(s) {}
        function reveal(p) {}
    }
    Component { id: factory; QuickExportWindow { controller: testController; inputPath: "C:/source.png"; preset: "texture" } }
    property var window: null
    function init() {
        service.busy=false; service.error=""; service.plan={}; service.job={}; service.jobs=[]; service.submissions=0; service.cancelledId=""
        window=factory.createObject(test)
        verify(window)
        compare(window.modality, Qt.NonModal)
        const source=findChild(window,"quickExportSource")
        const header=findChild(window,"topBar")
        tryVerify(function() { return source.mapToItem(null,0,0).y >= header.mapToItem(null,0,header.height).y })
    }
    function cleanup() { window.destroy(); wait(0) }
    function begin() { window.start({}, {}, "{name}.{ext}", "skip", {}) }
    function ready(losses) {
        service.plan={id:"plan-A",state:"ready",approval_digest:"digest-A",items:[{status:"ready",preflight:{losses:losses}}]}
        service.busy=false;service.changed();wait(0)
    }
    function test_lossless_one_click() {
        begin();ready([])
        tryCompare(service,"submissions",1)
        service.job={id:"A",state:"running",items:[]};service.busy=false;service.submissionFinished("plan-A","A");service.changed()
        tryCompare(window,"ownedJob","A")
        service.jobs=[{id:"A",state:"running",items:[]},{id:"B",state:"succeeded",items:[]}]
        service.selectJob("B");wait(0)
        const pane=findChild(window,"quickExportJob")
        compare(pane.job.id,"A")
        findChild(window,"cancelImageJob").clicked()
        compare(service.cancelledId,"A")
        compare(service.job.id,"B")
        // list_jobs summaries may remain unchanged while a pinned item finishes.
        service.jobs[0].items=[{status:"succeeded"}]
        tryCompare(pane,"finished",1)
    }
    function test_loss_requires_acknowledgement() {
        begin();ready([{code:"test_loss",detail:"Synthetic loss"}]);wait(0)
        compare(service.submissions,0)
        const button=findChild(window,"submitImagePlan")
        verify(!button.enabled)
        findChild(window,"acknowledgeImageLosses").checked=true
        verify(button.enabled)
        button.clicked()
        compare(service.submissions,1)
    }
    function test_closed_preflight_cannot_submit() {
        begin();window.close();ready([])
        compare(service.submissions,0)
        compare(service.cancelledId,"preparation")
    }
    function test_submission_response_is_pinned_even_if_selection_changed() {
        begin();ready([]);tryCompare(service,"submissions",1)
        service.jobs=[{id:"A",state:"running",items:[]},{id:"B",state:"succeeded",items:[]}]
        service.job=service.jobs[1];service.busy=false
        service.submissionFinished("plan-A","A");service.changed();wait(0)
        compare(window.ownedJob,"A")
        compare(findChild(window,"quickExportJob").job.id,"A")
    }
    function test_close_during_submission_cancels_exact_job() {
        begin();ready([]);tryCompare(service,"submissions",1)
        window.close()
        verify(window.visible)
        service.job={id:"A",state:"running",items:[]};service.busy=false;service.submissionFinished("plan-A","A");service.changed()
        tryCompare(window,"visible",false)
        compare(service.cancelledId,"A")
    }
    function test_old_ready_plan_is_not_submitted() {
        service.plan={id:"old",state:"ready",items:[{status:"ready",preflight:{losses:[]}}]}
        // Simulate a preflight rejected before it replaces an existing draft.
        window.previousPlan="old";window.phase="preflight";window.autoContinue=true
        service.error="scope busy";service.changed();wait(0)
        compare(service.submissions,0)
        compare(window.ownedPlan,"")
    }
}
