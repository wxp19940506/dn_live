package com.dongnaoedu.live;

import com.dongnaoedu.live.pusher.LivePusher;

import android.app.Activity;
import android.os.Bundle;
import android.view.SurfaceView;
import android.view.View;
import android.widget.Button;

public class MainActivity extends Activity {

	static final String URL = "rtmp://192.168.1.117/live/demo";
	private LivePusher live;

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
		setContentView(R.layout.activity_main);
		SurfaceView surfaceView = (SurfaceView) findViewById(R.id.surface);
		//相机图像的预览
		live = new LivePusher(surfaceView.getHolder());
	}

	/**
	 * 开始直播
	 * @param btn
	 */
	public void mStartLive(View view) {
		Button btn = (Button)view;
		if(btn.getText().equals("开始直播")){
			live.startPush(URL);
			btn.setText("停止直播");
		}else{
			live.stopPush();
			btn.setText("开始直播");
		}
	}

	/**
	 * 切换摄像头
	 * @param btn
	 */
	public void mSwitchCamera(View btn) {
		live.switchCamera();
	}

}
