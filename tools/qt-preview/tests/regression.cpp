#include <QGuiApplication>
#include <QQuickWindow>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQmlApplicationEngine>
#include <QQmlProperty>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <QProcess>
#include <QElapsedTimer>
#include <QThread>
#include <QMetaProperty>
#include <QProcessEnvironment>
#include <QCryptographicHash>
#include <QtTest/QTest>
#include <private/qqmlpreviewclient_p.h>
#include <filesystem>
#include <fstream>
#include <functional>

QByteArray readBytes(const QString &path) {
    std::ifstream input(std::filesystem::path(path.toStdWString()), std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    return QByteArray::fromStdString(bytes);
}
bool writeBytes(const QString &path, const QByteArray &bytes) {
    std::ofstream output(std::filesystem::path(path.toStdWString()), std::ios::binary | std::ios::trunc);
    output.write(bytes.constData(), bytes.size());
    return output.good();
}
QString ptr(const QObject *object) { return QString::number(reinterpret_cast<quintptr>(object),16); }
QJsonObject properties(QObject *object) {
    QJsonObject result;
    if (!object) return result;
    result["pointer"]=ptr(object);
    for (const auto *name : {"text","marker","themeColor","observedTheme","epoch","plainCount","arrowCount",
             "connectionsCount","externalBlockCount","externalArrowCount","internalCount","keysCount",
             "lastPulseEpoch","lastKeyEpoch","externalBlockValue","externalArrowValue","functionValue"}) {
        const auto value=object->property(name);
        if (value.isValid()) result[name]=QJsonValue::fromVariant(value);
    }
    return result;
}
int targetMain(int argc,char **argv) {
    QQuickStyle::setStyle("Basic");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QGuiApplication app(argc,argv);
    const auto root=QDir::fromNativeSeparators(QString::fromLocal8Bit(argv[2]));
    QQmlApplicationEngine engine;
    QJsonArray warnings;
    QObject::connect(&engine,&QQmlEngine::warnings,&app,[&](const QList<QQmlError> &errors) {
        for (const auto &error:errors) warnings.append(error.toString());
    });
    engine.load(QUrl::fromLocalFile(root+"/Main.qml"));
    if (engine.rootObjects().isEmpty()) return 2;
    auto *object=engine.rootObjects().front();
    auto *window=qobject_cast<QQuickWindow *>(object);
    QTimer poll;
    int acknowledged=0;
    QObject::connect(&poll,&QTimer::timeout,&app,[&] {
        const auto command=QJsonDocument::fromJson(readBytes(root+"/command.json")).object();
        const auto sequence=command["sequence"].toInt();
        if (sequence>acknowledged) {
            const auto action=command["action"].toString();
            if (action=="quit") { app.quit(); return; }
            if (action=="pulse") {
                if (auto *signalTarget=object->findChild<QQuickItem *>("signal-target")) {
                    QMetaObject::invokeMethod(signalTarget,"plainPing",Q_ARG(int,7));
                    QMetaObject::invokeMethod(signalTarget,"arrowPing",Q_ARG(int,7));
                    QMetaObject::invokeMethod(signalTarget,"pulse",Q_ARG(int,7));
                    signalTarget->forceActiveFocus();
                    QTest::keyClick(window,Qt::Key_Escape);
                }
            }
            if (action=="convert" || action=="assets") {
                if (action=="assets") object->setProperty("page","assets");
                else {
                    const auto walk=[&](QQuickItem *node,auto &&self)->void {
                        if(node->objectName()=="convertImagesButton") QMetaObject::invokeMethod(node,"clicked");
                        for(auto *child:node->childItems())self(child,self);
                    };
                    walk(window->contentItem(),walk);
                }
            }
            acknowledged=sequence;
        }
        QJsonObject snapshot{{"pid",app.applicationPid()},{"root",ptr(object)},
            {"acknowledged",acknowledged},{"page",object->property("page").toString()},
            {"properties",properties(object)},{"warnings",warnings}};
        QJsonArray items;
        const auto walk=[&](QQuickItem *node,auto &&self)->void {
            if(node->property("pressed").isValid() || node->objectName()=="signal-target") {
                auto item=properties(node);item["name"]=node->objectName();
                const auto role=node->metaObject()->indexOfProperty("modelData");
                item["role_type"]=role<0?QString():QString::fromLatin1(node->metaObject()->property(role).typeName());
                items.append(item);
            }
            for(auto *child:node->childItems())self(child,self);
        };
        walk(window->contentItem(),walk);
        snapshot["items"]=items;
        QJsonObject identities;
        for(const auto *name:{"signal-target","external-observers","external-block","external-arrow","external-function"})
            if(auto *child=object->findChild<QObject *>(name)) identities[name]=ptr(child);
        snapshot["identities"]=identities;
        snapshot["observers"]=properties(object->findChild<QObject *>("external-observers"));
        if(auto *theme=object->property("themeObject").value<QObject *>()) {
            QQmlProperty action(theme,"action");
            snapshot["theme_pointer"]=ptr(theme);
            snapshot["theme_value"]=action.read().value<QColor>().name();
            snapshot["theme_readonly"]=!action.isWritable();
            snapshot["external_theme_write_rejected"]=!action.write(QColor("#00ff00"));
        }
        const auto statePath=root+"/state.json";
        if(writeBytes(statePath+".new",QJsonDocument(snapshot).toJson())) {
            std::error_code error;
            std::filesystem::rename(std::filesystem::path((statePath+".new").toStdWString()),
                                    std::filesystem::path(statePath.toStdWString()),error);
        }
    });
    poll.start(30);
    return app.exec();
}
int main(int argc,char **argv) {
    if(argc>1 && QByteArray(argv[1])=="--target") return targetMain(argc,argv);
    QCoreApplication app(argc,argv);
    if(argc!=4)return 64;
    const auto root=QDir::fromNativeSeparators(app.arguments()[1]);
    const auto fixtures=QDir::fromNativeSeparators(app.arguments()[2]);
    const auto caseName=app.arguments()[3];
    if(QFileInfo::exists(root))return 65;
    if(!QDir().mkpath(root))return 66;
    const auto config=QJsonDocument::fromJson(readBytes(fixtures+"/case.json")).object();
    const auto changed=config["changed_file"].toString();
    for(const auto &name:QDir(fixtures+"/base").entryList(QDir::Files))
        if(!writeBytes(root+"/"+name,readBytes(fixtures+"/base/"+name)))return 68;
    const auto a=readBytes(fixtures+"/A.qml"),b=readBytes(fixtures+"/B.qml");
    if(a.isEmpty()||b.isEmpty()||changed.isEmpty())return 69;
    writeBytes(root+"/"+changed,a);
    QQmlDebugConnection connection;
    QQmlPreviewClient client(&connection);
    QJsonArray errors,hotFailures;
    bool confirmed=false;
    QObject::connect(&client,&QQmlPreviewClient::request,&app,[&](const QString &path) {
        if(QFileInfo(path).isDir())client.sendDirectory(path,QDir(path).entryList(QDir::AllEntries|QDir::NoDotAndDotDot));
        else {const auto bytes=readBytes(path);if(!bytes.isEmpty())client.sendFile(path,bytes);else client.sendError(path);}
    });
    QObject::connect(&client,&QQmlPreviewClient::stateChanged,&app,[&](QQmlDebugClient::State state) {
        if(state==QQmlDebugClient::Enabled)client.sendConfiguration({true});
    });
    QObject::connect(&client,&QQmlPreviewClient::confirmation,&app,[&](const QQmlPreviewClient::Settings &settings) {confirmed=settings.enableInPlaceUpdates;});
    QObject::connect(&client,&QQmlPreviewClient::error,&app,[&](const QString &error){errors.append(error);});
    QObject::connect(&client,&QQmlPreviewClient::hotReloadFailure,&app,[&](const QString &error){hotFailures.append(error);});
    const auto socket=root+"/owned-preview-"+QString::number(app.applicationPid());
    connection.startLocalServer(socket);
    QProcess child;
    auto environment=QProcessEnvironment::systemEnvironment();
    environment.insert("FORMAVIEW_HOTRELOAD_TRACE",root+"/trace.jsonl");
    child.setProcessEnvironment(environment);
    child.setProgram(app.applicationFilePath());
    child.setArguments({"--target",root,"-platform","offscreen","-qmljsdebugger=file:"+socket+",block,services:QmlPreview"});
    child.setStandardOutputFile(root+"/target-stdout.log");
    child.setStandardErrorFile(root+"/target-stderr.log");
    child.start();if(!child.waitForStarted(5000))return 67;
    const auto processId=child.processId();
    QJsonObject lastState;
    const auto state=[&]{
        const auto next=QJsonDocument::fromJson(readBytes(root+"/state.json")).object();
        if(!next.isEmpty())lastState=next;
        return lastState;
    };
    const auto wait=[&](const std::function<bool()> &predicate,int timeout=6000) {
        QElapsedTimer timer;timer.start();
        while(timer.elapsed()<timeout && child.state()!=QProcess::NotRunning) {
            QCoreApplication::processEvents();if(predicate())return true;QThread::msleep(5);
        }
        return false;
    };
    const auto settle=[&](int duration=200) {
        QElapsedTimer timer;timer.start();
        while(timer.elapsed()<duration && child.state()!=QProcess::NotRunning) {QCoreApplication::processEvents();QThread::msleep(5);}
    };
    int commandSequence=0;
    const auto command=[&](const QString &action) {
        ++commandSequence;
        writeBytes(root+"/command.json",QJsonDocument(QJsonObject{{"sequence",commandSequence},{"action",action}}).toJson());
        return wait([&]{return state()["acknowledged"].toInt()==commandSequence;});
    };
    QJsonObject report{{"case",caseName},{"target_pid",processId},{"driver_pid",app.applicationPid()},
        {"a_sha256",QString::fromLatin1(QCryptographicHash::hash(a,QCryptographicHash::Sha256).toHex())},
        {"b_sha256",QString::fromLatin1(QCryptographicHash::hash(b,QCryptographicHash::Sha256).toHex())},
        {"changed_file",changed},{"sync_error","not applicable: dedicated file protocol"}};
    QJsonArray stages;
    const auto itemProperty=[&](const QString &property) {
        for(const auto &value:state()["items"].toArray()) {
            const auto item=value.toObject();
            if(item["name"]=="convertImagesButton"||item["name"]=="signal-target")return item[property];
        }
        return QJsonValue();
    };
    const auto observed=[&](const QString &phase) {
        if(caseName=="text")return itemProperty("text").toString()==(phase=="B"?QString::fromUtf8("转换·47"):QString::fromUtf8("转换"));
        if(caseName=="theme")return state()["theme_value"].toString()==(phase=="B"?"#1976d2":"#c74c0a")
             &&state()["properties"].toObject()["themeColor"].toString()==state()["theme_value"].toString();
        if(caseName=="structure")return itemProperty("role_type").toString()==(phase=="B"?"QString":"QVariant");
        if(caseName=="handlers") {
            if(config["observed_object"]=="external-observers")return state()["observers"].toObject()["epoch"].toString()==phase;
            return itemProperty("epoch").toString()==phase;
        }
        return !state().isEmpty();
    };
    const auto assess=[&](const QString &label) {
        QJsonObject stage{{"label",label},{"before",state()}};
        if(caseName=="handlers") {
            stage["command_completed"]=command("pulse");settle();stage["after"]=state();
        } else {
            stage["command_completed"]=command("convert");settle();stage["navigation"]=state()["page"]=="conversion";
            stage["after"]=state();command("assets");settle();
        }
        stages.append(stage);
        writeBytes(root+"/stages.json",QJsonDocument(stages).toJson());
    };
    report["confirmed_in_place"]=wait([&]{return confirmed&&observed("A");});
    report["baseline"]=state();
    if(report["confirmed_in_place"].toBool()) {
        assess("baseline");
        const int rounds=config["rounds"].toInt(1);
        bool proceed=true;
        for(int round=0;round<rounds&&proceed;++round) for(const auto &phase:{QString("B"),QString("A")}) {
            const auto bytes=phase=="B"?b:a;
            const auto errorsBefore=errors.size()+hotFailures.size();
            writeBytes(root+"/phase",(QString::number(round)+"-"+phase).toUtf8());
            writeBytes(root+"/"+changed,bytes);client.sendFile(root+"/"+changed,bytes);
            client.triggerLoad(QUrl::fromLocalFile(root+"/Main.qml"));
            const bool changedObserved=wait([&]{
                if(caseName=="invalid"&&phase=="B")return errors.size()+hotFailures.size()>errorsBefore;
                return observed(phase);
            });settle();
            if(!changedObserved) {report["first_unobserved"]=QString::number(round)+"-"+phase;proceed=false;break;}
            assess(QString::number(round)+"-"+phase);
        }
    }
    report["stages"]=stages;
    report["errors"]=errors;report["hot_reload_failures"]=hotFailures;report["final"]=state();
    report["a_restored_bytes"]=readBytes(root+"/"+changed)==a;
    command("quit");wait([&]{return child.state()==QProcess::NotRunning;},1000);
    if(child.state()!=QProcess::NotRunning)child.terminate();
    child.waitForFinished(1000);
    if(child.state()!=QProcess::NotRunning){child.kill();child.waitForFinished(1000);report["owned_child_timeout"]=true;}
    report["crashed"]=child.exitStatus()==QProcess::CrashExit;report["exit_code"]=child.exitCode();
    writeBytes(root+"/result.json",QJsonDocument(report).toJson());
    return 0;
}
