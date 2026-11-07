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
        signal directExportFinished(string jobId)
        property bool busy: false
        property var preparation: ({})
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
        property int directSubmissions: 0
        property string cancelledId: ""
        function nameExamples() { return {names:["source-converted.png"]} }
        function invalidatePlan() { plan = {}; changed() }
        function preflight() { busy = true; plan = {}; changed() }
        function submit(ack) { submissions++; busy = true; job = {}; changed() }
        function startExport() { directSubmissions++; busy = true; job = {}; changed() }
        function cancelPreparation() { cancelledId = "preparation"; preparation = Object.assign({}, preparation, {cancellation_requested:true}); changed() }
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
    Component {
        id: taskPaneFactory
        ApplicationWindow {
            visible: true; width: 900; height: 700
            ImageJobsPane { objectName: "taskCenterPane"; controller: testController; anchors.fill: parent }
        }
    }
    property var window: null
    property string artifactRoot: ""
    function init() {
        service.busy=false; service.preparation={}; service.error=""; service.plan={}; service.job={}; service.jobs=[]; service.submissions=0; service.directSubmissions=0; service.cancelledId=""
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
    function test_close_during_submission_keeps_submitted_job() {
        begin();ready([]);tryCompare(service,"submissions",1)
        window.close()
        verify(window.visible)
        service.job={id:"A",state:"running",items:[]};service.busy=false;service.submissionFinished("plan-A","A");service.changed()
        tryCompare(window,"visible",false)
        compare(window.ownedJob,"A")
        compare(service.cancelledId,"")
        compare(service.job.state,"running")
    }
    function test_direct_export_result_is_owned() {
        window.preset="animation"
        begin(); compare(service.directSubmissions,1); compare(window.phase,"direct")
        service.job={id:"direct-A",state:"running",items:[]};service.busy=false
        service.directExportFinished("direct-A");service.changed();wait(0)
        compare(window.ownedJob,"direct-A")
        compare(window.phase,"job")
    }
    function test_preparation_feedback_data() { return [{tag:"model",preset:"model"},{tag:"animation",preset:"animation"}] }
    function test_preparation_feedback(data) {
        window.preset=data.preset
        window.inputPath="C:/source.fbx"
        begin()
        service.preparation={active:true,stage:"inspect",input:"C:/source.fbx",completed:0,total:0,elapsed_ms:4200,cancellation_requested:false}
        const pane=findChild(window,"quickExportJob")
        const progress=findChild(pane,"exportPreparationProgress")
        verify(progress, "Preparation must have a visible activity indicator")
        tryCompare(progress,"visible",true)
        verify(progress.indeterminate)
        verify(!findChild(pane,"noConversionJob").visible)
        verify(findChild(pane,"exportPreparationElapsed").text.includes("4"))
        if (artifactRoot) {
            wait(300); grabImage(window.contentItem).save(artifactRoot+"/reading-"+data.preset+"-1.png")
            wait(300); grabImage(window.contentItem).save(artifactRoot+"/reading-"+data.preset+"-2.png")
        }
        service.preparation=Object.assign({},service.preparation,{stage:"record",completed:12,total:138,elapsed_ms:6000})
        tryCompare(progress,"indeterminate",false)
        compare(progress.value,12); compare(progress.to,138)
        wait(50)
        if (artifactRoot) grabImage(window.contentItem).save(artifactRoot+"/preparation-"+data.preset+".png")
        const cancel=findChild(pane,"cancelExportPreparation")
        verify(cancel.enabled); cancel.clicked()
        compare(service.cancelledId,"preparation"); verify(!cancel.enabled)
        service.busy=false;service.preparation={active:false};service.error="cancelled: Preparation cancelled"
        // A queued cancellation has no worker callback; the error notification
        // alone must also return the window to its settings.
        if (data.preset === "model") service.directExportFinished("")
        service.changed()
        tryCompare(window,"phase","settings")
        verify(!progress.visible)
    }
    function test_pinned_job_does_not_show_other_preparation() {
        service.job={id:"A",state:"succeeded",items:[]};window.ownedJob="A";window.phase="job"
        service.busy=true;service.preparation={active:true,stage:"inspect",elapsed_ms:4000}
        const pane=findChild(window,"quickExportJob")
        const progress=findChild(pane,"exportPreparationProgress")
        verify(progress); verify(!progress.visible)
        verify(!findChild(pane,"cancelExportPreparation").visible)
        compare(pane.job.id,"A")
    }
    function test_task_center_can_cancel_background_preparation() {
        window.preset="animation"; begin(); window.close()
        service.preparation={active:true,stage:"protect",elapsed_ms:6000,cancellation_requested:false}
        service.job={id:"old",state:"succeeded",items:[]}
        const taskWindow=createTemporaryObject(taskPaneFactory,test)
        verify(taskWindow)
        const pane=findChild(taskWindow,"taskCenterPane")
        tryCompare(findChild(pane,"exportPreparationProgress"),"visible",true)
        findChild(pane,"cancelExportPreparation").clicked()
        compare(service.cancelledId,"preparation")
        compare(pane.job.id,"old")
    }
    function test_close_during_direct_export_does_not_cancel() {
        window.preset="animation"
        begin(); compare(service.directSubmissions,1)
        window.close(); tryCompare(window,"visible",false)
        compare(service.cancelledId,"")
        service.job={id:"direct-A",state:"running",items:[]};service.busy=false
        service.directExportFinished("direct-A");service.changed();wait(0)
        compare(service.job.state,"running")
        compare(service.cancelledId,"")
    }
    function test_old_ready_plan_is_not_submitted() {
        service.plan={id:"old",state:"ready",items:[{status:"ready",preflight:{losses:[]}}]}
        // Simulate a preflight rejected before it replaces an existing draft.
        window.previousPlan="old";window.phase="preflight";window.autoContinue=true
        service.error="scope busy";service.changed();wait(0)
        compare(service.submissions,0)
        compare(window.ownedPlan,"")
    }
    function exportItem(index, name, status) {
        return {index:index, status:status, request:{input:"C:/source.fbx", output:"C:/"+name+".fbx", options:{target:"fbx",include_mesh:false}},
            result:{clips:[{name:name,start:0,end:1}],output:"C:/"+name+".fbx"}}
    }
    function test_details_survive_progress_and_other_submission() {
        const a = {id:"A",state:"running",items:[exportItem(0,"Walk","running")]}
        service.job=a; service.jobs=[a]; window.ownedJob="A"; window.phase="job"
        const pane=findChild(window,"quickExportJob")
        tryVerify(() => !!findChild(pane,"conversionItemDetails"))
        findChild(pane,"conversionItemDetails").clicked()
        const details=findChild(pane,"conversionDetails")
        if (!details) {
            // Reproduce the old delegate resetting expanded on each snapshot.
            const row=findChild(pane,"conversionItemDetails").parent.parent.parent
            verify(row.expanded)
            service.jobs=[{id:"A",state:"succeeded",items:[exportItem(0,"Walk","succeeded")]}]
            service.job=service.jobs[0]; service.changed(); wait(50)
            verify(findChild(pane,"conversionItemDetails").parent.parent.parent.expanded,
                   "Progress refresh must not close the selected details")
            return
        }
        tryCompare(details,"opened",true)
        service.jobs=[{id:"A",state:"running",items:[exportItem(0,"Walk","running")]},
                      {id:"B",state:"running",items:[exportItem(0,"Run","running")]}]
        service.selectJob("B"); wait(250)
        verify(details.opened); compare(details.entry.result.clips[0].name,"Walk")
        // A newly submitted batch may become the visible batch; details stay pinned.
        window.ownedJob="B"; wait(50)
        verify(details.opened); compare(details.entry.result.clips[0].name,"Walk")
        service.jobs=[{id:"A",state:"succeeded",items:[exportItem(0,"Walk","succeeded")]},service.jobs[1]]
        service.changed()
        tryCompare(details,"entry",service.jobs[0].items[0])
        verify(details.opened)
        if (artifactRoot) grabImage(window.contentItem).save(artifactRoot+"/persistent-details.png")
        findChild(details,"closeConversionDetails").clicked(); tryCompare(details,"opened",false)
        findChild(pane,"conversionItemDetails").forceActiveFocus()
        keyClick(Qt.Key_Space); tryCompare(details,"opened",true)
        keyClick(Qt.Key_Escape); tryCompare(details,"opened",false)
        verify(window.visible)
        findChild(pane,"conversionItemDetails").clicked(); tryCompare(details,"opened",true)
        service.jobs=[service.jobs[0]]; service.job=service.jobs[0]; service.changed()
        tryCompare(details,"opened",false)
    }
    function test_dense_searchable_large_batch() {
        window.preset="animation"; window.inputPath="C:/source.fbx"
        const items=[]
        for (let i=0;i<138;++i) items.push(exportItem(i,"Clip_"+i,i%2 ? "failed" : "succeeded"))
        // Clip names must be searchable independently of generated filenames.
        items[137].result.clips[0].name="Unique Walk Cycle"
        service.jobs=[{id:"A",state:"succeeded",items:items}];service.job=service.jobs[0]
        window.ownedJob="A";window.phase="job";window.width=760
        const pane=findChild(window,"quickExportJob")
        tryCompare(pane,"items",items)
        const list=findChild(pane,"imageJobsTable")
        tryVerify(() => !!list.itemAtIndex(0))
        verify(list.itemAtIndex(0).height <= 76, "Compact row must not grow with repeated metadata")
        const search=findChild(pane,"exportItemSearch")
        search.text="unique walk"
        tryCompare(list,"count",1);compare(pane.shownItems[0].index,137)
        findChild(pane,"jobShowFailures").clicked();tryCompare(list,"count",1)
        search.text="Clip_136";tryCompare(list,"count",0)
        findChild(pane,"jobShowAll").clicked();tryCompare(list,"count",1)
        search.clear();tryCompare(list,"count",138)
        wait(50)
        const detailsButton=findChild(list.itemAtIndex(0),"conversionItemDetails")
        verify(detailsButton.mapToItem(window.contentItem,detailsButton.width,0).x <= window.width,
               "Details action must fit the narrow window")
        if (artifactRoot) grabImage(window.contentItem).save(artifactRoot+"/compact-760.png")
        window.width=1100;wait(50)
        verify(list.itemAtIndex(0).height <= 76)
        if (artifactRoot) grabImage(window.contentItem).save(artifactRoot+"/compact-1100.png")
    }
}
