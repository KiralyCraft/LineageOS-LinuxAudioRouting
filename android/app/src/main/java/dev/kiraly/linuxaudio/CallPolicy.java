package dev.kiraly.linuxaudio;

import android.Manifest;
import android.content.pm.PackageManager;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.telephony.PhoneStateListener;
import android.telephony.TelephonyManager;

final class CallPolicy implements AutoCloseable
{
	private final AudioService service;
	private final AudioManager audio;
	private final TelephonyManager phone;
	private final PhoneStateListener phoneListener;
	private final AudioManager.OnModeChangedListener modeListener;
	private final AudioFocusRequest focus;
	private volatile boolean phoneCall;
	private volatile boolean focusLost;
	private volatile boolean ownsCommunication;
	private boolean phoneRegistered;
	private boolean focusRequested;

	CallPolicy(AudioService service)
	{
		this.service = service;
		audio = service.getSystemService(AudioManager.class);
		phone = service.getSystemService(TelephonyManager.class);
		focus = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN).setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build()).setWillPauseWhenDucked(true).setOnAudioFocusChangeListener(change -> {
																																																															   focusLost = change != AudioManager.AUDIOFOCUS_GAIN;
																																																															   update();
																																																														   })
					.build();
		phoneListener = new PhoneStateListener() {
			@Override
			public void onCallStateChanged(int state, String ignoredNumber)
			{
				phoneCall = state != TelephonyManager.CALL_STATE_IDLE;
				update();
			}
		};
		modeListener = mode -> update();
		audio.addOnModeChangedListener(service.getMainExecutor(), modeListener);
		if (service.checkSelfPermission(Manifest.permission.READ_PHONE_STATE) == PackageManager.PERMISSION_GRANTED)
		{
			phone.listen(phoneListener, PhoneStateListener.LISTEN_CALL_STATE);
			phoneRegistered = true;
		}
	}

	boolean priorityCall()
	{
		int mode = audio.getMode();
		return phoneCall || mode == AudioManager.MODE_IN_CALL || (mode == AudioManager.MODE_IN_COMMUNICATION && !ownsCommunication);
	}

	synchronized boolean acquire()
	{
		if (priorityCall())
			return false;
		if (!focusRequested)
		{
			int result = audio.requestAudioFocus(focus);
			if (result != AudioManager.AUDIOFOCUS_REQUEST_GRANTED)
				return false;
			focusRequested = true;
			focusLost = false;
		}
		return !focusLost;
	}

	synchronized boolean headset(android.media.AudioDeviceInfo device)
	{
		if (!acquire() || priorityCall())
			return false;
		ownsCommunication = true;
		audio.setMode(AudioManager.MODE_IN_COMMUNICATION);
		if (!audio.setCommunicationDevice(device))
		{
			releaseCommunication();
			return false;
		}
		return true;
	}

	synchronized void releaseCommunication()
	{
		if (ownsCommunication)
		{
			audio.clearCommunicationDevice();
			audio.setMode(AudioManager.MODE_NORMAL);
			ownsCommunication = false;
		}
	}

	private void update()
	{
		boolean suspended = priorityCall() || focusLost;
		if (suspended)
			releaseCommunication();
		service.priorityChanged(suspended, suspended ? "Android call or audio-focus priority" : "");
	}

	synchronized void idle()
	{
		releaseCommunication();
		if (focusRequested)
		{
			audio.abandonAudioFocusRequest(focus);
			focusRequested = false;
			focusLost = false;
		}
	}

	@Override
	public synchronized void close()
	{
		idle();
		audio.removeOnModeChangedListener(modeListener);
		if (phoneRegistered)
			phone.listen(phoneListener, PhoneStateListener.LISTEN_NONE);
	}
}
