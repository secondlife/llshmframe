#include "cef_consumer.h"
#include "../cef_protocol.h"

#include <cstdlib>
#include <iostream>

using namespace cef_demo;

CefConsumer::CefConsumer() :
    mWindow(nullptr),
    mTextureWidth(kDefaultWidth),
    mTextureHeight(kDefaultHeight),
    mTextureId(0)
{
}

CefConsumer::~CefConsumer() = default;

static void errorCallback(int error, const char* description)
{
    std::cout << description << " - code: " << error << std::endl;
    exit(1);
}

bool CefConsumer::connectToProducer(int slot_count, const std::string& start_url)
{
    bool saw_any_producer = false;

    for (int i = 0; i < slot_count; ++i)
    {
        auto sub = LLSubscriber::open(kChannelPrefix + std::to_string(i));
        if (!sub->connected()) continue;

        saw_any_producer = true;
        if (sub->owns_command_channel())
        {
            // The producer always starts a channel at kDefaultWidth x
            // kDefaultHeight, matching this class's constructor defaults, so
            // there's nothing further to size here -- the window opens at
            // that size and the first resizeCallback() reconciles it with
            // whatever the OS actually grants the window.
            mSub       = std::move(sub);
            mSlotIndex = i;
            std::cout << "connected to slot " << i << "\n";

            // Stands in for the start URL a real CEF embedder always
            // supplies when it creates a browser view -- sent immediately so
            // the producer's first regeneration already reflects it rather
            // than a placeholder default.
            if (!start_url.empty())
            {
                std::cout << "-> kSetUrl " << start_url << "\n";
                mSub->send_text(kSetUrl, start_url);
            }
            return true;
        }
    }

    if (!saw_any_producer)
        std::cerr << "no cef producer found (is llshmframe_cef_producer running?)\n";
    else
        std::cerr << "producer is full: all " << slot_count << " channels are already claimed\n";
    return false;
}

void CefConsumer::keyCallback(int key, int scancode, int action, int mods)
{
    (void)scancode; (void)mods;
    if (action != GLFW_PRESS) return;

    if (key == GLFW_KEY_ESCAPE)
    {
        glfwSetWindowShouldClose(mWindow, GLFW_TRUE);
    }
    else if (key == 'R') { std::cout << "-> kSetUrl red\n";   mSub->send_text(kSetUrl, "red");   }
    else if (key == 'G') { std::cout << "-> kSetUrl green\n"; mSub->send_text(kSetUrl, "green"); }
    else if (key == 'B') { std::cout << "-> kSetUrl blue\n";  mSub->send_text(kSetUrl, "blue");  }
}

void CefConsumer::mouseButtonCallback(int button, int action, int mods)
{
    (void)mods;
    double mx, my;
    glfwGetCursorPos(mWindow, &mx, &my);

    std::uint8_t payload[10];
    const std::uint32_t n = pack_mouse_button(payload, int(mx), int(my),
                                              std::uint8_t(button), std::uint8_t(action));
    mSub->send(kMouseButton, payload, n);
}

void CefConsumer::mouseMoveCallback(double xpos, double ypos)
{
    // Coalesced, not sent here: GLFW can deliver far more of these than the
    // command ring should carry. sendPendingMouseMove() flushes at most one
    // per rendered frame from run().
    mPendingMoveX = xpos;
    mPendingMoveY = ypos;
    mMoveDirty    = true;
}

void CefConsumer::sendPendingMouseMove()
{
    if (!mMoveDirty) return;
    mMoveDirty = false;

    std::uint8_t payload[8];
    const std::uint32_t n = pack_i32x2(payload, int(mPendingMoveX), int(mPendingMoveY));
    mSub->send(kMouseMove, payload, n);
}

void CefConsumer::resizeCallback(int width, int height)
{
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0f, width, 0.0f, height, -1.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW);
    glViewport(0, 0, width, height);

    mTextureWidth  = width;
    mTextureHeight = height;

    glDeleteTextures(1, &mTextureId);
    glGenTextures(1, &mTextureId);
    glBindTexture(GL_TEXTURE_2D, mTextureId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // llshmframe's pixel convention is BGRA (LLConfig::bytes_per_pixel's
    // documented default, and what the producer actually writes) -- the
    // *source* format below must say so, even though the GL_RGBA internal
    // format (storage on the GPU) doesn't need to match it.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mTextureWidth, mTextureHeight, 0, GL_BGRA, GL_UNSIGNED_BYTE, 0);

    mHaveFrame = false; // last frame's dims no longer match; wait for a fresh one

    // Ask the producer to reshape this channel's canvas to match -- the
    // shmframe geometry is bounded by whatever max_width/max_height the
    // producer configured for this channel, so an oversized window just
    // gets clamped server-side rather than rejected.
    std::uint8_t payload[8];
    const std::uint32_t n = pack_size(payload, std::uint32_t(width), std::uint32_t(height));
    mSub->send(kResize, payload, n);
}

void CefConsumer::initGLFWCallbacks()
{
    glfwSetKeyCallback(mWindow, keyCallbackStatic);
    glfwSetMouseButtonCallback(mWindow, mouseButtonCallbackStatic);
    glfwSetCursorPosCallback(mWindow, mouseMoveCallbackStatic);

    int width, height;
    glfwSetFramebufferSizeCallback(mWindow, resizeCallbackStatic);
    glfwGetFramebufferSize(mWindow, &width, &height);
    resizeCallback(width, height);
}

void CefConsumer::init()
{
    if (! glfwInit())
    {
        exit(EXIT_FAILURE);
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_SAMPLES, 0);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);

    const std::string title = mWindowTitle + " - slot " + std::to_string(mSlotIndex);
    mWindow = glfwCreateWindow(mTextureWidth, mTextureHeight, title.c_str(), nullptr, nullptr);
    if (! mWindow)
    {
        glfwTerminate();
        exit(EXIT_FAILURE);
    }

    glfwSetWindowPos(mWindow, 64 + mSlotIndex * 24, 100 + mSlotIndex * 24);

    glfwSetWindowUserPointer(mWindow, this);

    glfwSetErrorCallback(errorCallback);
    glfwMakeContextCurrent(mWindow);
    glfwSwapInterval(1);
    gladLoadGL();

    glEnable(GL_TEXTURE_2D);

    initGLFWCallbacks();
}

void CefConsumer::update()
{
    const bool connected_now = mSub->connected();
    if (connected_now != mWasConnected)
    {
        mWasConnected = connected_now;
        std::cout << "slot " << mSlotIndex << ": " << (connected_now ? "connected" : "disconnected") << "\n";
    }

    const LLReadResult r = mSub->read_latest(mFrameBuf, mFrameInfo);
    if (r == LLReadResult::Ok)
    {
        // Only upload when the frame matches the texture's current size --
        // during a live resize the producer's reply lags the request by a
        // round trip, so a mismatched frame is simply skipped rather than
        // risking an out-of-bounds copy. The window shows its last valid
        // frame until sizes agree again.
        mHaveFrame = (mFrameInfo.width == mTextureWidth && mFrameInfo.height == mTextureHeight);
    }
}

void CefConsumer::draw()
{
    if (mHaveFrame)
    {
        glBindTexture(GL_TEXTURE_2D, mTextureId);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, mTextureWidth, mTextureHeight,
                        GL_BGRA, GL_UNSIGNED_BYTE, mFrameBuf.data());
    }

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glLoadIdentity();

    glColor3f(1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);

    glTexCoord2f(1.0f, 0.0f);
    glVertex2d(mTextureWidth, mTextureHeight);

    glTexCoord2f(0.0f, 0.0f);
    glVertex2d(0, mTextureHeight);

    glTexCoord2f(0.0f, 1.0f);
    glVertex2d(0, 0);

    glTexCoord2f(1.0f, 1.0f);
    glVertex2d(mTextureWidth, 0);

    glEnd();
}

void CefConsumer::run()
{
    while (! glfwWindowShouldClose(mWindow))
    {
        update();

        draw();

        glfwSwapBuffers(mWindow);

        glfwPollEvents();

        sendPendingMouseMove();
    }
}

void CefConsumer::reset()
{
    glDeleteTextures(1, &mTextureId);
    mTextureId = 0;

    glfwDestroyWindow(mWindow);

    glfwTerminate();
}

int main(int argc, char* argv[])
{
    int slot_count = kSlotCount;
    if (argc > 1) slot_count = std::atoi(argv[1]);
    if (slot_count <= 0) slot_count = 1;

    const std::string start_url = argc > 2 ? argv[2] : "";

    CefConsumer app;
    if (! app.connectToProducer(slot_count, start_url))
    {
        return 1;
    }

    app.init();
    app.run();
    app.reset();

    return 0;
}
