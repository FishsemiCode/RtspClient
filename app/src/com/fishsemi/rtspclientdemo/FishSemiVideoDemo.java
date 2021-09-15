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

package com.fishsemi.rtspclientdemo;

import android.app.Activity;
import android.graphics.SurfaceTexture;
import android.os.Bundle;
import android.view.Surface;
import android.view.TextureView;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;

import com.fishsemi.sdk.aircontrol.VideoStream;

public class FishSemiVideoDemo extends Activity {
    private static final String TAG = "FishSemiVideoDemo";
    private boolean mShowPushLayout = true;
    VideoStream mVideoStream;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        setContentView(R.layout.activity_main);
        TextureView tv = (TextureView) this.findViewById(R.id.texture_view);

        tv.setSurfaceTextureListener(new TextureView.SurfaceTextureListener() {
            @Override
            public void onSurfaceTextureAvailable(SurfaceTexture surfaceTexture, int i, int i1) {
                mVideoStream.setSurface(new Surface(surfaceTexture));
            }

            @Override
            public void onSurfaceTextureSizeChanged(SurfaceTexture surfaceTexture, int i, int i1) {

            }

            @Override
            public boolean onSurfaceTextureDestroyed(SurfaceTexture surfaceTexture) {
                return false;
            }

            @Override
            public void onSurfaceTextureUpdated(SurfaceTexture surfaceTexture) {

            }
        });

        Button playButton = (Button) this.findViewById(R.id.play);
        playButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                EditText editTextMediaUrl = (EditText) findViewById(R.id.media_url);
                String mediaUrl = editTextMediaUrl.getText().toString();
                if (mediaUrl.isEmpty()) {
                    mediaUrl ="rtsp://192.168.0.10:8554/H264Video";;
                    mVideoStream.setStreamUrl(mediaUrl);
                }
                mVideoStream.play();
            }
        });

        Button stopButton = (Button) this.findViewById(R.id.stop);
        stopButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                mVideoStream.stop();
            }
        });

        if (mShowPushLayout) {
            findViewById(R.id.push_layout).setVisibility(View.VISIBLE);
            Button pushButton = (Button) findViewById(R.id.push);
            pushButton.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View view) {
                    if (!mVideoStream.isPushingVideoStream()) {
                        EditText serverUrl = (EditText) findViewById(R.id.push_server_url);
                        String pushServerUrl = serverUrl.getText().toString();
                        if (pushServerUrl.startsWith("rtmp://")) {
                            mVideoStream.setRtmpPushServerUrl(pushServerUrl);
                        } else if (pushServerUrl.startsWith("rtsp://")) {
                            mVideoStream.setRtspPushServerUrl(pushServerUrl);
                        }
                        mVideoStream.startPushVideoStream();
                    }
                }
            });
            Button stopPushButton = (Button) findViewById(R.id.stop_push);
            stopPushButton.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View view) {
                    if (mVideoStream.isPushingVideoStream()) {
                        mVideoStream.stopPushVideoStream();
                    }
                }
            });
        }
        mVideoStream = new VideoStream(this, "rtsp://192.168.0.10:8554/H264Video", null);
    }
}
