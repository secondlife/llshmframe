/**
 *
 * @file multiview_consumer.cpp
 * @brief MultiviewConsumer implementation: control-channel handshake, GLFW window, and input callbacks
 *
 * $LicenseInfo:firstyear=2023&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2023, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "multiview_consumer.h"
#include "../multiview_protocol.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace multiview_demo;

namespace {
    constexpr auto kControlClaimRetryInterval = std::chrono::milliseconds(50);
    constexpr auto kControlClaimTimeout       = std::chrono::seconds(3); // > the library's ~2s steal window
    constexpr auto kSlotRequestTimeout        = std::chrono::seconds(2);
    constexpr auto kSlotReplyPollInterval     = std::chrono::milliseconds(5);
}

MultiviewConsumer::MultiviewConsumer() :
    mWindow(nullptr),
    mTextureWidth(kDefaultWidth),
    mTextureHeight(kDefaultHeight),
    mTextureId(0)
{
}

MultiviewConsumer::~MultiviewConsumer() = default;

static void errorCallback(int error, const char* description)
{
    std::cout << description << " - code: " << error << std::endl;
    exit(1);
}

bool MultiviewConsumer::connectToProducer(const std::string& start_url)
{
    // Claim the control channel. A losing race must destroy this
    // LLSubscriber and open() a fresh one to retry -- poll() on an
    // already-connected instance can never re-attempt the claim, it only
    // re-validates the session it already has. A prior requester that
    // crashed mid-request self-heals via the library's stale-heartbeat
    // steal, so this retry loop just needs to outlast that (~2s) window.
    std::unique_ptr<LLSubscriber> ctrl;
    const auto claim_deadline = std::chrono::steady_clock::now() + kControlClaimTimeout;
    for (;;)
    {
        ctrl = LLSubscriber::open(kControlChannelName);
        if (!ctrl->connected())
        {
            std::cerr << "no multiview producer found (is llshmframe_multiview_producer running?)\n";
            return false;
        }
        if (ctrl->owns_command_channel()) break;

        if (std::chrono::steady_clock::now() >= claim_deadline)
        {
            std::cerr << "producer's control channel is busy, try again\n";
            return false;
        }
        ctrl.reset();
        std::this_thread::sleep_for(kControlClaimRetryInterval);
    }

    std::uint64_t req_id = 0;
    if (!ctrl->send(kRequestSlot, nullptr, 0, 0, &req_id))
    {
        std::cerr << "failed to request a view from the producer\n";
        return false;
    }

    LLCommand reply;
    bool got_reply = false;
    const auto reply_deadline = std::chrono::steady_clock::now() + kSlotRequestTimeout;
    while (std::chrono::steady_clock::now() < reply_deadline)
    {
        if (ctrl->receive(reply) && reply.reply_to == req_id) { got_reply = true; break; }
        std::this_thread::sleep_for(kSlotReplyPollInterval);
    }
    if (!got_reply)
    {
        std::cerr << "producer did not respond to the view request\n";
        return false;
    }

    std::uint32_t index = 0;
    if (reply.type != kSlotAssigned || !unpack_u32(reply.data.data(), reply.data.size(), index))
    {
        std::cerr << "producer has no free view right now\n";
        return false;
    }

    ctrl.reset(); // release the control claim for the next requester

    auto sub = LLSubscriber::open(kChannelPrefix + std::to_string(index));
    if (!sub->connected() || !sub->owns_command_channel())
    {
        std::cerr << "view " << index << " was assigned but could not be claimed\n";
        return false;
    }

    // The producer always starts a channel at kDefaultWidth x kDefaultHeight,
    // matching this class's constructor defaults, so there's nothing further
    // to size here -- the window opens at that size and the first
    // resizeCallback() reconciles it with whatever the OS actually grants
    // the window.
    mSub       = std::move(sub);
    mSlotIndex = int(index);
    std::cout << "connected to slot " << mSlotIndex << "\n";

    // Stands in for the start URL a real CEF embedder always supplies when
    // it creates a browser view -- sent immediately so the producer's first
    // regeneration already reflects it rather than a placeholder default.
    if (!start_url.empty())
    {
        std::cout << "-> kSetUrl " << start_url << "\n";
        mSub->send_text(kSetUrl, start_url);
    }
    return true;
}

void MultiviewConsumer::keyCallback(int key, int scancode, int action, int mods)
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

void MultiviewConsumer::mouseButtonCallback(int button, int action, int mods)
{
    (void)mods;
    double mx, my;
    glfwGetCursorPos(mWindow, &mx, &my);

    std::uint8_t payload[10];
    const std::uint32_t n = pack_mouse_button(payload, int(mx), int(my),
                                              std::uint8_t(button), std::uint8_t(action));
    mSub->send(kMouseButton, payload, n);
}

void MultiviewConsumer::mouseMoveCallback(double xpos, double ypos)
{
    // Coalesced, not sent here: GLFW can deliver far more of these than the
    // command ring should carry. sendPendingMouseMove() flushes at most one
    // per rendered frame from run().
    mPendingMoveX = xpos;
    mPendingMoveY = ypos;
    mMoveDirty    = true;
}

void MultiviewConsumer::sendPendingMouseMove()
{
    if (!mMoveDirty) return;
    mMoveDirty = false;

    std::uint8_t payload[8];
    const std::uint32_t n = pack_i32x2(payload, int(mPendingMoveX), int(mPendingMoveY));
    mSub->send(kMouseMove, payload, n);
}

void MultiviewConsumer::resizeCallback(int width, int height)
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

void MultiviewConsumer::initGLFWCallbacks()
{
    glfwSetKeyCallback(mWindow, keyCallbackStatic);
    glfwSetMouseButtonCallback(mWindow, mouseButtonCallbackStatic);
    glfwSetCursorPosCallback(mWindow, mouseMoveCallbackStatic);

    int width, height;
    glfwSetFramebufferSizeCallback(mWindow, resizeCallbackStatic);
    glfwGetFramebufferSize(mWindow, &width, &height);
    resizeCallback(width, height);
}

void MultiviewConsumer::init()
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

void MultiviewConsumer::update()
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

void MultiviewConsumer::draw()
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

void MultiviewConsumer::run()
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

void MultiviewConsumer::reset()
{
    glDeleteTextures(1, &mTextureId);
    mTextureId = 0;

    glfwDestroyWindow(mWindow);

    glfwTerminate();
}

int main(int argc, char* argv[])
{
    const std::string start_url = argc > 1 ? argv[1] : "";

    MultiviewConsumer app;
    if (! app.connectToProducer(start_url))
    {
        return 1;
    }

    app.init();
    app.run();
    app.reset();

    return 0;
}
