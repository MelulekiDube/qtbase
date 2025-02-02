/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the plugins of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qoffscreenintegration_x11.h"

#include <QByteArray>
#include <QOpenGLContext>

#include <X11/Xlib.h>
#include <GL/glx.h>

#include <QtGui/private/qglxconvenience_p.h>

#include <qpa/qplatformsurface.h>
#include <qsurface.h>

QT_BEGIN_NAMESPACE

class QOffscreenX11Info
{
public:
    QOffscreenX11Info(QOffscreenX11Connection *connection)
        : m_connection(connection)
    {
    }

    Display *display() const {
        return (Display *)m_connection->display();
    }

    Window root() const {
        return DefaultRootWindow(display());
    }

    int screenNumber() const {
        return m_connection->screenNumber();
    }

private:
    QOffscreenX11Connection *m_connection;
};

QOffscreenX11Integration::QOffscreenX11Integration(const QStringList& paramList)
: QOffscreenIntegration(paramList)
{

}

QOffscreenX11Integration::~QOffscreenX11Integration() = default;

bool QOffscreenX11Integration::hasCapability(QPlatformIntegration::Capability cap) const
{
    switch (cap) {
    case OpenGL: return true;
    case ThreadedOpenGL: return true;
    case RasterGLSurface: return true;
    default: return QOffscreenIntegration::hasCapability(cap);
    }
}

#if !defined(QT_NO_OPENGL) && QT_CONFIG(xcb_glx_plugin)
QPlatformOpenGLContext *QOffscreenX11Integration::createPlatformOpenGLContext(QOpenGLContext *context) const
{
    auto &connection = nativeInterface()->m_connection;

    if (!connection)
        connection.reset(new QOffscreenX11Connection);

    if (!connection->display())
        return nullptr;

    return new QOffscreenX11GLXContext(connection->x11Info(), context);
}
#endif // !defined(QT_NO_OPENGL) && QT_CONFIG(xcb_glx_plugin)

QOffscreenX11PlatformNativeInterface *QOffscreenX11Integration::nativeInterface() const
{
   if (!m_nativeInterface)
       m_nativeInterface.reset(new QOffscreenX11PlatformNativeInterface(const_cast<QOffscreenX11Integration *>(this)));
   return static_cast<QOffscreenX11PlatformNativeInterface *>(m_nativeInterface.data());
}

QOffscreenX11PlatformNativeInterface::QOffscreenX11PlatformNativeInterface(QOffscreenIntegration *integration)
:QOffscreenPlatformNativeInterface(integration)
{

}

QOffscreenX11PlatformNativeInterface::~QOffscreenX11PlatformNativeInterface() = default;

void *QOffscreenX11PlatformNativeInterface::nativeResourceForScreen(const QByteArray &resource, QScreen *screen)
{
    Q_UNUSED(screen);
    if (resource.toLower() == QByteArrayLiteral("display") ) {
        if (!m_connection)
            m_connection.reset(new QOffscreenX11Connection);
        return m_connection->display();
    }
    return nullptr;
}

#if !defined(QT_NO_OPENGL) && QT_CONFIG(xcb_glx_plugin)
void *QOffscreenX11PlatformNativeInterface::nativeResourceForContext(const QByteArray &resource, QOpenGLContext *context) {
    if (resource.toLower() == QByteArrayLiteral("glxconfig") ) {
        if (context) {
            QOffscreenX11GLXContext *glxPlatformContext = static_cast<QOffscreenX11GLXContext *>(context->handle());
            if (glxPlatformContext)
                return glxPlatformContext->glxConfig();
        }
    }
    if (resource.toLower() == QByteArrayLiteral("glxcontext") ) {
        if (context) {
            QOffscreenX11GLXContext *glxPlatformContext = static_cast<QOffscreenX11GLXContext *>(context->handle());
            if (glxPlatformContext)
                return glxPlatformContext->glxContext();
        }
    }
    return nullptr;
}
#endif

QOffscreenX11Connection::QOffscreenX11Connection()
{
    XInitThreads();

    QByteArray displayName = qgetenv("DISPLAY");
    Display *display = XOpenDisplay(displayName.constData());
    m_display = display;
    m_screenNumber = m_display ? DefaultScreen(m_display) : -1;
}

QOffscreenX11Connection::~QOffscreenX11Connection()
{
    if (m_display)
        XCloseDisplay((Display *)m_display);
}

QOffscreenX11Info *QOffscreenX11Connection::x11Info()
{
    if (!m_x11Info)
        m_x11Info.reset(new QOffscreenX11Info(this));
    return m_x11Info.data();
}

#if QT_CONFIG(xcb_glx_plugin)
class QOffscreenX11GLXContextData
{
public:
    QOffscreenX11Info *x11 = nullptr;
    QSurfaceFormat format;
    GLXContext context = nullptr;
    GLXContext shareContext = nullptr;
    GLXFBConfig config = nullptr;
    Window window = 0;
};

static Window createDummyWindow(QOffscreenX11Info *x11, XVisualInfo *visualInfo)
{
    Colormap cmap = XCreateColormap(x11->display(), x11->root(), visualInfo->visual, AllocNone);
    XSetWindowAttributes a;
    a.background_pixel = WhitePixel(x11->display(), x11->screenNumber());
    a.border_pixel = BlackPixel(x11->display(), x11->screenNumber());
    a.colormap = cmap;


    Window window = XCreateWindow(x11->display(), x11->root(),
                                  0, 0, 100, 100,
                                  0, visualInfo->depth, InputOutput, visualInfo->visual,
                                  CWBackPixel|CWBorderPixel|CWColormap, &a);
    XFreeColormap(x11->display(), cmap);
    return window;
}

static Window createDummyWindow(QOffscreenX11Info *x11, GLXFBConfig config)
{
    XVisualInfo *visualInfo = glXGetVisualFromFBConfig(x11->display(), config);
    if (Q_UNLIKELY(!visualInfo))
        qFatal("Could not initialize GLX");
    Window window = createDummyWindow(x11, visualInfo);
    XFree(visualInfo);
    return window;
}

QOffscreenX11GLXContext::QOffscreenX11GLXContext(QOffscreenX11Info *x11, QOpenGLContext *context)
    : d(new QOffscreenX11GLXContextData)
{

    d->x11 = x11;
    d->format = context->format();

    if (d->format.renderableType() == QSurfaceFormat::DefaultRenderableType)
        d->format.setRenderableType(QSurfaceFormat::OpenGL);

    if (d->format.renderableType() != QSurfaceFormat::OpenGL)
        return;

    d->shareContext = nullptr;
    if (context->shareHandle())
        d->shareContext = static_cast<QOffscreenX11GLXContext *>(context->shareHandle())->d->context;

    GLXFBConfig config = qglx_findConfig(x11->display(), x11->screenNumber(), d->format);
    d->config = config;

    if (config) {
        d->context = glXCreateNewContext(x11->display(), config, GLX_RGBA_TYPE, d->shareContext, true);
        if (!d->context && d->shareContext) {
            d->shareContext = nullptr;
            // re-try without a shared glx context
            d->context = glXCreateNewContext(x11->display(), config, GLX_RGBA_TYPE, nullptr, true);
        }

        // Get the basic surface format details
        if (d->context)
            qglx_surfaceFormatFromGLXFBConfig(&d->format, x11->display(), config);

        // Create a temporary window so that we can make the new context current
        d->window = createDummyWindow(x11, config);
    } else {
        XVisualInfo *visualInfo = qglx_findVisualInfo(x11->display(), 0, &d->format);
        if (Q_UNLIKELY(!visualInfo))
            qFatal("Could not initialize GLX");
        d->context = glXCreateContext(x11->display(), visualInfo, d->shareContext, true);
        if (!d->context && d->shareContext) {
            // re-try without a shared glx context
            d->shareContext = nullptr;
            d->context = glXCreateContext(x11->display(), visualInfo, nullptr, true);
        }

        d->window = createDummyWindow(x11, visualInfo);
        XFree(visualInfo);
    }
}

QOffscreenX11GLXContext::~QOffscreenX11GLXContext()
{
    glXDestroyContext(d->x11->display(), d->context);
    XDestroyWindow(d->x11->display(), d->window);
}

bool QOffscreenX11GLXContext::makeCurrent(QPlatformSurface *surface)
{
    QSize size = surface->surface()->size();

    XResizeWindow(d->x11->display(), d->window, size.width(), size.height());
    XSync(d->x11->display(), true);

    if (glXMakeCurrent(d->x11->display(), d->window, d->context)) {
        glViewport(0, 0, size.width(), size.height());
        return true;
    }

    return false;
}

void QOffscreenX11GLXContext::doneCurrent()
{
    glXMakeCurrent(d->x11->display(), 0, nullptr);
}

void QOffscreenX11GLXContext::swapBuffers(QPlatformSurface *)
{
}

QFunctionPointer QOffscreenX11GLXContext::getProcAddress(const char *procName)
{
    return (QFunctionPointer)glXGetProcAddressARB(reinterpret_cast<const GLubyte *>(procName));
}

QSurfaceFormat QOffscreenX11GLXContext::format() const
{
    return d->format;
}

bool QOffscreenX11GLXContext::isSharing() const
{
    return d->shareContext;
}

bool QOffscreenX11GLXContext::isValid() const
{
    return d->context && d->window;
}

GLXContext QOffscreenX11GLXContext::glxContext() const
{
    return d->context;
}

void *QOffscreenX11GLXContext::glxConfig() const
{
    return d->config;
}
#endif // QT_CONFIG(xcb_glx_plugin)
QT_END_NAMESPACE
