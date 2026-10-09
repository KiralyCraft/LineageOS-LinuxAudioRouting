package dev.kiraly.linuxaudio;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.util.ArrayList;

public final class MainActivity extends Activity
{
	private final Handler handler = new Handler();
	private TextView status;
	private final Runnable refresh = new Runnable() {
		public void run()
		{
			status.setText(AudioService.statusText);
			handler.postDelayed(this, 1000);
		}
	};

	@Override
	public void onCreate(Bundle saved)
	{
		super.onCreate(saved);
		ScrollView scroll = new ScrollView(this);
		LinearLayout layout = new LinearLayout(this);
		layout.setOrientation(LinearLayout.VERTICAL);
		layout.setPadding(36, 56, 36, 36);
		scroll.addView(layout);
		TextView title = new TextView(this);
		title.setText("Linux Audio");
		title.setTextSize(32);
		title.setTextColor(Color.rgb(89, 218, 199));
		layout.addView(title);
		TextView description = new TextView(this);
		description.setText("Choose devices and volume in Linux. Android handles phone calls normally.\n\nStart once after reboot. The helper continues working with the phone screen off.");
		description.setTextSize(17);
		description.setPadding(0, 24, 0, 24);
		layout.addView(description);
		status = new TextView(this);
		status.setTextSize(17);
		status.setPadding(20, 24, 20, 24);
		status.setBackgroundColor(Color.rgb(28, 43, 54));
		layout.addView(status);
		Button start = new Button(this);
		start.setText("Start Linux audio");
		start.setOnClickListener(view -> permissionsAndStart());
		layout.addView(start);
		Button stop = new Button(this);
		stop.setText("Stop");
		stop.setOnClickListener(view -> stopService(new Intent(this, AudioService.class)));
		layout.addView(stop);
		TextView privacy = new TextView(this);
		privacy.setText("The microphone opens only while a Linux application records. Phone state is used to suspend Linux audio during calls; call audio and phone numbers are not recorded. Linux volume does not change Android's media or call volume.");
		privacy.setPadding(0, 24, 0, 0);
		layout.addView(privacy);
		setContentView(scroll);
		if (getIntent().getBooleanExtra("start", false))
			permissionsAndStart();
	}

	private void permissionsAndStart()
	{
		ArrayList<String> missing = new ArrayList<>();
		for (String permission : new String[] {Manifest.permission.RECORD_AUDIO, Manifest.permission.BLUETOOTH_CONNECT, Manifest.permission.READ_PHONE_STATE, Manifest.permission.POST_NOTIFICATIONS})
			if (checkSelfPermission(permission) != PackageManager.PERMISSION_GRANTED)
				missing.add(permission);
		if (missing.isEmpty())
			startForegroundService(new Intent(this, AudioService.class));
		else
			requestPermissions(missing.toArray(new String[0]), 1);
	}

	@Override
	public void onRequestPermissionsResult(int request, String[] permissions, int[] grants)
	{
		super.onRequestPermissionsResult(request, permissions, grants);
		if (request == 1)
			startForegroundService(new Intent(this, AudioService.class));
	}

	@Override
	public void onResume()
	{
		super.onResume();
		handler.post(refresh);
	}
	@Override
	public void onPause()
	{
		handler.removeCallbacks(refresh);
		super.onPause();
	}
}
