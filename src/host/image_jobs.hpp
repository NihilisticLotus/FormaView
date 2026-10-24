#pragma once
#include "formaview/image_batch.hpp"
#include "formaview/image_endpoint.hpp"
#include "formaview/scheduler.hpp"
#include <QObject>
#include <QTimer>
#include <QVariantMap>

class ImageJobs final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(QVariantMap capabilities READ capabilities CONSTANT)
  Q_PROPERTY(bool configured READ configured NOTIFY changed)
  Q_PROPERTY(QString error READ error NOTIFY changed)
  Q_PROPERTY(QVariantMap plan READ plan NOTIFY changed)
  Q_PROPERTY(QVariantMap job READ job NOTIFY changed)
  Q_PROPERTY(QVariantList jobs READ jobs NOTIFY changed)
  Q_PROPERTY(QVariantMap scope READ scope NOTIFY changed)
  Q_PROPERTY(bool mcpEnabled READ mcpEnabled NOTIFY changed)
  Q_PROPERTY(QString endpoint READ endpoint NOTIFY changed)
  Q_PROPERTY(QVariantMap lastClient READ lastClient NOTIFY changed)
  Q_PROPERTY(QVariantList inputInfo READ inputInfo NOTIFY changed)
  Q_PROPERTY(QVariantList modelSources READ modelSources NOTIFY changed)
  Q_PROPERTY(QVariantMap discovery READ discovery NOTIFY changed)
  Q_PROPERTY(int discoveryOffset READ discoveryOffset NOTIFY changed)
public:
  explicit ImageJobs(QObject *parent = nullptr);
  ~ImageJobs() override;
  bool busy() const { return busy_; }
  QVariantMap capabilities() const {
    return formaview::imaging::image_capabilities().toVariantMap();
  }
  bool configured() const { return !!service_; }
  QString error() const { return error_; }
  QVariantMap plan() const { return plan_; }
  QVariantMap job() const { return job_; }
  QVariantList jobs() const { return jobs_; }
  QVariantMap scope() const { return scope_; }
  bool mcpEnabled() const { return !!endpoint_; }
  QString endpoint() const { return endpointName_; }
  QVariantMap lastClient() const { return lastClient_; }
  QVariantList inputInfo() const { return inputInfo_; }
  QVariantList modelSources() const { return modelSources_; }
  Q_INVOKABLE void inspectModelInputs(const QStringList &inputs, const QVariantMap &scope, const QString &paired);
  QVariantMap discovery() const { return discovery_; }
  int discoveryOffset() const { return discoveryOffset_; }
  Q_INVOKABLE void configureScope(const QVariantMap &scope);
  Q_INVOKABLE void discover(const QString &directory, bool recursive);
  Q_INVOKABLE void discoveryPage(int offset);
  Q_INVOKABLE void setMcpEnabled(bool);
  Q_INVOKABLE QString mcpConfiguration(const QString &python,
                                       const QString &adapter);
  Q_INVOKABLE void
  preflight(const QStringList &inputs, const QVariantMap &scope,
            const QVariantMap &options, const QString &nameTemplate,
            const QString &conflict, const QVariantList &itemOverrides = {});
  Q_INVOKABLE void submit(bool acknowledge);
  Q_INVOKABLE void cancel(const QString &kind, const QString &id);
  Q_INVOKABLE void retry(const QString &id);
  Q_INVOKABLE void selectJob(const QString &id);
  Q_INVOKABLE void invalidatePlan();
  Q_INVOKABLE QVariantMap nameExamples(const QStringList &inputs,
                                      const QString &nameTemplate,
                                      const QString &target,
                                      const QVariantList &itemOverrides) const;
  Q_INVOKABLE void cancelPreparation();
signals:
  void changed();

private:
  void dispatch(QJsonObject);
  void poll();
  bool activeJobs() const;
  formaview::Scheduler control_;
  std::shared_ptr<formaview::imaging::ImageBatchService> service_;
  QTimer timer_;
  QVariantMap scope_, plan_, job_;
  QVariantList jobs_;
  QVariantList inputInfo_;
  QVariantList modelSources_;
  QString error_, planId_, jobId_, idempotency_;
  bool busy_{};
  quint64 revision_{};
  std::optional<formaview::TaskHandle> preparation_;
  std::unique_ptr<formaview::imaging::ImageEndpoint> endpoint_;
  std::vector<std::unique_ptr<formaview::imaging::ImageEndpoint>>
      retiredEndpoints_;
  QString endpointName_;
  QVariantMap lastClient_;
  QVariantMap discovery_;
  QString discoveryId_;
  int discoveryOffset_{};
};
