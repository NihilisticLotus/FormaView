#pragma once
class Desktop;
class QQuickWindow;
class VideoSession;
class QString;

// A live window uses its actual player. An owned stdio session runs the same
// decoder and desktop asset pipeline without constructing a window.
void attachDesktopMcp(Desktop &, QQuickWindow *, VideoSession *background = nullptr);
bool startScopedEndpoint(Desktop &, const QString &scopePath);
bool watchMcpParent(unsigned long parentPid);
int runBackgroundMcp(const QString &scopePath, unsigned long parentPid);
