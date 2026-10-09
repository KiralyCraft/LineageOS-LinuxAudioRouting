package dev.kiraly.linuxaudio;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import org.json.JSONArray;
import org.json.JSONObject;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.HashMap;
import java.util.Map;

final class Devices
{
	final Context context;
	final AudioManager manager;
	private final Map<String, AudioDeviceInfo> devices = new HashMap<>();
	private final Map<String, JSONObject> descriptors = new HashMap<>();

	Devices(Context context)
	{
		this.context = context;
		manager = context.getSystemService(AudioManager.class);
	}

	static String digest(String value) throws Exception
	{
		byte[] hash = MessageDigest.getInstance("SHA-256").digest(value.getBytes(StandardCharsets.UTF_8));
		StringBuilder text = new StringBuilder();
		for (int i = 0; i < 12; ++i)
			text.append(String.format("%02x", hash[i] & 255));
		return text.toString();
	}

	static boolean supported(int type)
	{
		switch (type)
		{
		case AudioDeviceInfo.TYPE_BUILTIN_SPEAKER:
		case AudioDeviceInfo.TYPE_BUILTIN_EARPIECE:
		case AudioDeviceInfo.TYPE_BUILTIN_MIC:
		case AudioDeviceInfo.TYPE_WIRED_HEADSET:
		case AudioDeviceInfo.TYPE_WIRED_HEADPHONES:
		case AudioDeviceInfo.TYPE_USB_HEADSET:
		case AudioDeviceInfo.TYPE_USB_DEVICE:
		case AudioDeviceInfo.TYPE_BLUETOOTH_A2DP:
		case AudioDeviceInfo.TYPE_BLUETOOTH_SCO:
		case AudioDeviceInfo.TYPE_BLE_HEADSET:
		case AudioDeviceInfo.TYPE_BLE_SPEAKER:
			return true;
		default:
			return false;
		}
	}

	static boolean bluetooth(AudioDeviceInfo device)
	{
		int type = device.getType();
		return type == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP || type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO || type == AudioDeviceInfo.TYPE_BLE_HEADSET || type == AudioDeviceInfo.TYPE_BLE_SPEAKER;
	}

	synchronized JSONArray refresh() throws Exception
	{
		devices.clear();
		descriptors.clear();
		JSONArray inventory = new JSONArray();
		boolean record = context.checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED;
		boolean nearby = context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED;
		for (AudioDeviceInfo device : manager.getDevices(AudioManager.GET_DEVICES_ALL))
		{
			if (!supported(device.getType()))
				continue;
			if (bluetooth(device) && !nearby)
				continue;
			boolean input = device.isSource();
			String direction = input ? "input" : "output";
			String address = device.getAddress();
			String name = device.getProductName().toString();
			String identity = address.isEmpty() ? name : address;
			String key = direction + "." + device.getType() + "." + digest(identity);
			if (devices.containsKey(key))
				continue;
			String group = digest(identity);
			String profile = "media";
			int rate = 48000;
			int channels = input ? 1 : 2;
			if (device.getType() == AudioDeviceInfo.TYPE_BLUETOOTH_SCO)
			{
				profile = "headset";
				rate = 8000;
				for (int supportedRate : device.getSampleRates())
					if (supportedRate == 16000)
						rate = 16000;
				channels = 1;
			}
			else if (device.getType() == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP)
				profile = "stereo";
			int[] counts = device.getChannelCounts();
			if (counts.length == 1 && counts[0] == 1)
				channels = 1;
			if (device.getType() == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER)
				name = "Phone Speakers";
			else if (device.getType() == AudioDeviceInfo.TYPE_BUILTIN_EARPIECE)
				name = "Phone Earpiece";
			else if (device.getType() == AudioDeviceInfo.TYPE_BUILTIN_MIC)
				name = "Phone Microphone " + address;
			else if (profile.equals("stereo"))
				name += " — Stereo";
			else if (profile.equals("headset"))
				name += input ? " — Microphone" : " — Headset";
			else if (input)
				name += " — Microphone";
			boolean available = (!input || record) && (!bluetooth(device) || nearby);
			String format = "f32le";
			if (profile.equals("headset"))
				format = "s16le";
			JSONObject descriptor = new JSONObject().put("key", key).put("group", group).put("name", name).put("direction", direction).put("android_id", device.getId()).put("type", device.getType()).put("profile", profile).put("rate", rate).put("channels", channels).put("format", format).put("clock_driver", true).put("shared_pcm", true).put("available", available).put("reason", available ? "" : "Android permission required");
			String resource = "playback.fast";
			if (profile.equals("stereo"))
				resource = "playback.buffered";
			if (input)
			{
				int source = android.media.MediaRecorder.AudioSource.VOICE_RECOGNITION;
				resource = "capture.voice-recognition";
				if ("true".equals(manager.getProperty(AudioManager.PROPERTY_SUPPORT_AUDIO_SOURCE_UNPROCESSED)))
				{
					source = android.media.MediaRecorder.AudioSource.UNPROCESSED;
					resource = "capture.raw";
				}
				if (profile.equals("headset"))
				{
					source = android.media.MediaRecorder.AudioSource.VOICE_COMMUNICATION;
					resource = "capture.communication";
				}
				descriptor.put("capture_source", source);
			}
			descriptor.put("resource", resource);
			if (device.getType() == AudioDeviceInfo.TYPE_BUILTIN_MIC)
				descriptor.put("selection_group", "phone-microphone");
			devices.put(key, device);
			descriptors.put(key, descriptor);
			inventory.put(descriptor);
		}
		return inventory;
	}

	synchronized AudioDeviceInfo communication(String group)
	{
		for (Map.Entry<String, JSONObject> entry : descriptors.entrySet())
		{
			JSONObject descriptor = entry.getValue();
			if (descriptor.optString("group").equals(group) && descriptor.optString("profile").equals("headset") && descriptor.optString("direction").equals("output"))
				return devices.get(entry.getKey());
		}
		return null;
	}

	synchronized AudioDeviceInfo get(String key)
	{
		return devices.get(key);
	}
	synchronized JSONObject describe(String key)
	{
		return descriptors.get(key);
	}
}
