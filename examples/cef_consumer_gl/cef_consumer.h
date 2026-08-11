#pragma once

#include <glad/glad.h>
#if defined(WIN32)
#undef APIENTRY
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <commctrl.h>
#else
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#endif

#include <shmframe/llshmframe.h>

#include <memory>
#include <string>
#include <vector>

// GLFW/OpenGL consumer for the CEF-style demo: probes the producer's fixed
// channel pool, claims one, and displays whatever it publishes as a single
// textured quad -- adapted from a plain local-pattern GLFW/OpenGL skeleton,
// with the pattern generation replaced by LLSubscriber::read_latest() and
// the input callbacks replaced by commands sent back to the producer.
//
// If the producer dies, this window does not attempt to wait around for it:
// connected() will go false and, once the producer comes back, this process
// is expected to have been closed and relaunched rather than reconnecting on
// its own. On Windows in particular, an old instance left running is exactly
// what blocks the producer from reclaiming its channel on restart -- see
// "Windows" in the top-level README.
class CefConsumer {
    public:
        CefConsumer();
        ~CefConsumer();

        // Probes llshmframe_cef_0.. for a channel nobody else has claimed,
        // then sends start_url as this view's initial kSetUrl if non-empty.
        // Must succeed before init()/run() are called.
        bool connectToProducer(int slot_count, const std::string& start_url = "");

        void init();
        void initGLFWCallbacks();
        void run();
        void reset();
        void update();
        void draw();

        void resizeCallback(int width, int height);
        void keyCallback(int key, int scancode, int action, int mods);
        void mouseButtonCallback(int button, int action, int mods);
        void mouseMoveCallback(double xpos, double ypos);

    private:
        void sendPendingMouseMove();

        GLFWwindow* mWindow;
        const std::string mWindowTitle = "llshmframe CEF Consumer";
        GLuint mTextureWidth;
        GLuint mTextureHeight;
        GLuint mTextureId;

        std::unique_ptr<LLSubscriber> mSub;
        int                           mSlotIndex = -1;
        std::vector<std::uint8_t>     mFrameBuf;
        LLFrameInfo                   mFrameInfo{};
        bool                          mHaveFrame  = false;
        bool                          mWasConnected = false;

        double mPendingMoveX = 0.0, mPendingMoveY = 0.0;
        bool   mMoveDirty    = false;

        // Used to marshall static function callbacks to a instance of the app class
        static void resizeCallbackStatic(GLFWwindow* window, int width, int height) {
            static_cast<CefConsumer*>(glfwGetWindowUserPointer(window))->resizeCallback(width, height);
        }
        static void keyCallbackStatic(GLFWwindow* window, int key, int scancode, int action, int mods) {
            static_cast<CefConsumer*>(glfwGetWindowUserPointer(window))->keyCallback(key, scancode, action, mods);
        }
        static void mouseButtonCallbackStatic(GLFWwindow* window, int button, int action, int mods) {
            static_cast<CefConsumer*>(glfwGetWindowUserPointer(window))->mouseButtonCallback(button, action, mods);
        }
        static void mouseMoveCallbackStatic(GLFWwindow* window, double xpos, double ypos) {
            static_cast<CefConsumer*>(glfwGetWindowUserPointer(window))->mouseMoveCallback(xpos, ypos);
        }
};
