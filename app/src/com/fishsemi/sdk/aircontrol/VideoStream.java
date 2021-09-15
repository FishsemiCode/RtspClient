/*
 * Copyright (C) 2021 FishSemi Inc. All rights reserved.

 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */

package com.fishsemi.sdk.aircontrol;

import android.content.Context;
import android.os.Handler;
import android.util.Log;
import android.view.Surface;

import org.freedesktop.gstreamer.GStreamer;

public class VideoStream {
    private static final String TAG = "VideoStream";
    private static final String RTMP_PUSH_STOP = "0: push rtmp branch shutdown";
    private static final String RTSP_PUSH_STOP = "1: push rtsp branch shutdown";
    private String mStreamUrl = null;
    private String mRtspPushUrl = null;
    private String mRtmpPushUrl = null;
    private VideoStreamListener mListener = null;
    private boolean isPlaying = false;
    private boolean isRtspPushing = false;
    private boolean isRtmpPushing = false;
    private boolean isSurfaceInited = false;
    private Handler mHandler = null;

    public VideoStream(Context context, String streamUrl, VideoStreamListener listener) {
        initLibraries(context);
        mStreamUrl = streamUrl;
        mListener = listener;
        mHandler = new Handler(context.getMainLooper());
        if (streamUrl != null) {
            mHandler.post(new Runnable() {
                @Override
                public void run() {
                    if (mListener != null) {
                        mListener.onPlayReady(true);
                    }
                }
            });
        }
    }

    public void setStreamUrl(String url) {
        if (url == null) {
            url = "";
        }
        if (mStreamUrl != null && mStreamUrl.equals(url)) {
            return;
        }
        setStreamUrlInternal(url);
    }

    public void setRtspPushServerUrl(String url) {
        if (url == null) {
            url = "";
        }
        mRtspPushUrl = url.trim();
        if (!mRtspPushUrl.startsWith("rtsp://")) {
            mRtspPushUrl = "";
        }
    }

    public void setRtmpPushServerUrl(String url) {
        if (url == null) {
            url = "";
        }
        mRtmpPushUrl = url.trim();
        if (!mRtmpPushUrl.startsWith("rtmp://")) {
            mRtmpPushUrl = "";
        }
    }

    public void setStreamUrlInternal(String url) {
        if (url != null) {
            if (mStreamUrl == null) {
                mHandler.post(new Runnable() {
                    @Override
                    public void run() {
                        if (mListener != null) {
                            mListener.onPlayReady(true);
                        }
                    }
                });
            }
            mStreamUrl = url;
            nativeSetRTSPURL(mStreamUrl);
        }
    }

    public void setSurface(Surface surface) {
        if (surface == null) {
            if (isSurfaceInited) {
                nativeFinalize();
                isSurfaceInited = false;
            }
        } else {
            nativeSurfaceInit(surface);
            isSurfaceInited = true;
        }
    }

    public void play() {
        if (isPlaying) {
            return;
        }
        if (mStreamUrl == null) {
            Log.e(TAG, "url is not set, play failed");
            return;
        }

        nativeSetRTSPURL(mStreamUrl);
        isPlaying = nativePlay();
        mHandler.post(new Runnable() {
            @Override
            public void run() {
                if (mListener != null) {
                    mListener.onPlayStateChanged(isPlaying);
                }
            }
        });
    }

    public void stop() {
        if (isPlaying) {
            isPlaying = false;
            nativeStop();
            mHandler.post(new Runnable() {
                @Override
                public void run() {
                    if (mListener != null) {
                        mListener.onPlayStateChanged(isPlaying);
                    }
                }
            });
        }
    }

    public boolean isPlaying() {
        return  isPlaying;
    }

    public void startPushVideoStream() {
        boolean urlSet = false;

        if (mStreamUrl == null) {
            Log.e(TAG, "rtsp source url is not set, push failed");
            return;
        }
        if (mRtmpPushUrl != null && mRtmpPushUrl.length() >= 0 && !isRtmpPushing) {
            nativeSetRTSPURL(mStreamUrl);
            urlSet = true;
            isRtmpPushing = nativePushStream(true, mRtmpPushUrl);
        }
        if (mRtspPushUrl != null && mRtspPushUrl.length() >= 0 && !isRtspPushing) {
            if (!urlSet) {
                nativeSetRTSPURL(mStreamUrl);
            }
            urlSet = true;
            isRtspPushing = nativePushStream(true, mRtspPushUrl);
        }

        mHandler.post(new Runnable() {
            @Override
            public void run() {
                if (mListener != null) {
                    mListener.onPushStateChanged(isRtmpPushing|isRtspPushing);
                }
            }
        });
    }

    public void stopPushVideoStream() {
        if (isRtmpPushing) {
            nativePushStream(false, mRtmpPushUrl);
            isRtmpPushing = false;
        }
        if (isRtspPushing) {
            nativePushStream(false, mRtspPushUrl);
            isRtspPushing = false;
        }

        mHandler.post(new Runnable() {
            @Override
            public void run() {
                if (mListener != null) {
                    mListener.onPushStateChanged(false);
                }
            }
        });
    }

    public boolean isPushingVideoStream() {
        return  isRtmpPushing | isRtspPushing;
    }

    protected void initLibraries(Context context) {
        System.loadLibrary("gstreamer_android");
        System.loadLibrary("songrtspclient");
        nativeClassInit();
        Log.d(TAG, "nativeClassInit done");
        try {
            GStreamer.init(context);
        } catch (Exception e) {
            Log.e(TAG, "initLibraries failed "+e);
            return;
        }
        if (!nativeInit()) {
            Log.e(TAG, "initLibraries failed on nativeInit");
        } else {
            Log.i(TAG, "initLibraries done. (custom data:" + native_custom_data + ")");
        }
    }

    protected void finalize () {
        nativeFinalize();
    }

    private void onGStreamerInitialized() {
        Log.i(TAG, "VideoStream surface has been set and thread is ready");
    }

    private void setMessage(final String message) {
        Log.d(TAG, message);
        if (RTMP_PUSH_STOP.equals(message)) {
            isRtmpPushing = false;
            mHandler.post(new Runnable() {
                @Override
                public void run() {
                    if (mListener != null) {
                        mListener.onPushStateChanged(isRtmpPushing|isRtspPushing);
                    }
                }
            });
        } else if (RTSP_PUSH_STOP.equals(message)) {
            isRtspPushing = false;
            mHandler.post(new Runnable() {
                @Override
                public void run() {
                    if (mListener != null) {
                        mListener.onPushStateChanged(isRtmpPushing|isRtspPushing);
                    }
                }
            });
        }
    }

    private void onMediaSizeChanged (int width, int height) {
        Log.d(TAG, "width:" + width + "height" + height);
    }

    // interfaces to native libs
    private long native_custom_data;
    private static native boolean nativeClassInit ();
    private native boolean nativeInit();
    private native void nativeFinalize();
    private native boolean nativePlay ();
    private native void nativeStop ();
    private native void nativeSurfaceInit (Object surface);
    private native void nativeSurfaceFinalize ();
    private native boolean nativeRecording(boolean enableRecording, String Dir);
    private native boolean nativePushStream(boolean startPushStream, String Url);
    private native void nativeSetRTSPURL(String mediaUrl);
    private native void nativeSetRTMPURL(String mediaUrl);
}
