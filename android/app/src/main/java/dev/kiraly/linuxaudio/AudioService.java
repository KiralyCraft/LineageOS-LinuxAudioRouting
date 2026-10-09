package dev.kiraly.linuxaudio;

import android.Manifest;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.media.AudioDeviceCallback;
import android.media.AudioDeviceInfo;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

public final class AudioService extends Service
{
	static final String STOP = "dev.kiraly.linuxaudio.STOP";
	static volatile String statusText = "Ready — start the helper after each reboot";
	private final RoutePolicy routes = new RoutePolicy();
	private final Map<Long, StreamIo> streams = new ConcurrentHashMap<>();
	private final Map<String, String> explicitProfiles = new ConcurrentHashMap<>();
	private final Handler main = new Handler(android.os.Looper.getMainLooper());
	private HandlerThread operationThread;
	private Handler operations;
	private Thread connectionThread;
	private volatile Wire connection;
	private volatile boolean running;
	volatile boolean suspended;
	private volatile String suspendReason = "";
	Devices devices;
	CallPolicy policy;
	private AudioDeviceCallback deviceCallback;
	private android.os.PowerManager.WakeLock wakeLock;

	@Override
	public void onCreate()
	{
		super.onCreate();
		wakeLock = getSystemService(android.os.PowerManager.class).newWakeLock(android.os.PowerManager.PARTIAL_WAKE_LOCK, "LinuxAudio:streams");
		wakeLock.setReferenceCounted(false);
		devices = new Devices(this);
		policy = new CallPolicy(this);
		operationThread = new HandlerThread("linux-audio-control");
		operationThread.start();
		operations = new Handler(operationThread.getLooper());
		deviceCallback = new AudioDeviceCallback() {
			@Override
			public void onAudioDevicesAdded(AudioDeviceInfo[] added)
			{
				devicesChanged();
			}
			@Override
			public void onAudioDevicesRemoved(AudioDeviceInfo[] removed)
			{
				for (StreamIo stream : streams.values())
					for (AudioDeviceInfo device : removed)
						if (stream.selected.getId() == device.getId())
							stream.close();
				devicesChanged();
			}
		};
		devices.manager.registerAudioDeviceCallback(deviceCallback, main);
	}

	@Override
	public int onStartCommand(Intent intent, int flags, int startId)
	{
		if (intent != null && STOP.equals(intent.getAction()))
		{
			stopSelf();
			return START_NOT_STICKY;
		}
		NotificationManager notifications = getSystemService(NotificationManager.class);
		notifications.createNotificationChannel(new NotificationChannel("linux-audio", "Linux audio connection", NotificationManager.IMPORTANCE_LOW));
		PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class), PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
		PendingIntent stop = PendingIntent.getService(this, 1, new Intent(this, AudioService.class).setAction(STOP), PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
		Notification notification = new Notification.Builder(this, "linux-audio").setSmallIcon(R.drawable.ic_audio).setContentTitle("Linux Audio").setContentText("Android handles calls; Linux controls its audio").setContentIntent(open).setOngoing(true).addAction(new Notification.Action.Builder(null, "Stop", stop).build()).build();
		int types = ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK;
		if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED)
			types |= ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE;
		startForeground(1, notification, types);
		if (!running)
		{
			running = true;
			connectionThread = new Thread(this::connect, "linux-audio-broker");
			connectionThread.start();
		}
		return START_NOT_STICKY;
	}

	private void connect()
	{
		while (running)
		{
			try (Wire link = new Wire())
			{
				link.hello("helper");
				link.socket.setSoTimeout(0);
				connection = link;
				status("Connected to Linux audio broker");
				operations.post(() -> { if (running && connection == link) { try { publishInventory(); } catch (Exception failure) { status("Discovery failed: " + failure.getMessage()); } } });
				while (running)
				{
					JSONObject request = link.receive();
					operations.post(() -> { if (running && connection == link) dispatch(request); });
				}
			}
			catch (Exception failure)
			{
				connection = null;
				closeStreams();
				operations.post(this::updatePolicy);
				if (running)
					status("Broker unavailable: " + failure.getMessage());
			}
			if (running)
			{
				try
				{
					Thread.sleep(1000);
				}
				catch (InterruptedException ignored)
				{
					break;
				}
			}
		}
	}

	private void dispatch(JSONObject request)
	{
		String operation = request.optString("op");
		if (operation.equals("close"))
		{
			StreamIo stream = streams.get(request.optLong("stream"));
			try
			{
				if (stream != null)
					retire(stream);
				updatePolicy();
				publishInventory();
			}
			catch (Exception failure)
			{
				status("Stream retirement failed: " + failure.getMessage());
			}
			return;
		}
		if (operation.equals("activate"))
		{
			StreamIo stream = streams.get(request.optLong("stream"));
			if (stream != null)
				stream.activate();
			return;
		}
		StreamIo stream = null;
		try
		{
			if (!running || suspended || policy.priorityCall())
				throw new IOException("Android call or focus priority");
			if (operation.equals("profile"))
			{
				profile(request.getString("endpoint"), request.getString("profile"));
				send(new JSONObject().put("op", "reply").put("id", request.getLong("id")).put("ok", true));
				return;
			}
			if (!operation.equals("open"))
				throw new IOException("Unknown broker operation");
			String endpoint = request.getString("endpoint");
			AudioDeviceInfo device = devices.get(endpoint);
			JSONObject descriptor = devices.describe(endpoint);
			if (device == null || descriptor == null || !descriptor.getBoolean("available"))
				throw new IOException("Selected endpoint disconnected or permission denied");
			String group = descriptor.getString("group");
			String explicit = explicitProfiles.get(group);
			if (explicit != null && (descriptor.getString("profile").equals("headset") || descriptor.getString("profile").equals("stereo")) && !explicit.equals(descriptor.getString("profile")))
				throw new IOException("Bluetooth profile is " + explicit + "; select the other profile before opening its route");
			String conflict = routes.conflict(endpoint, descriptor.getString("resource"));
			if (!conflict.isEmpty())
				throw new IOException(conflict);
			if (descriptor.getString("profile").equals("headset"))
			{
				for (StreamIo other : streams.values())
				{
					JSONObject otherDevice = devices.describe(other.endpoint);
					if (otherDevice != null && otherDevice.optString("group").equals(group) && !other.headset)
						retire(other);
				}
			}
			updatePolicy();
			stream = new StreamIo(this, request, device, descriptor);
			routes.reserve(stream.id, endpoint, descriptor.getString("resource"), descriptor.getString("name"), stream.headset);
			if (!policy.acquire())
				throw new IOException("Android denied audio focus");
			streams.put(stream.id, stream);
			updateWakeLock();
			stream.prepare(request);
			if (suspended || policy.priorityCall())
				throw new IOException("Android call started during setup");
			send(new JSONObject().put("op", "reply").put("id", request.getLong("id")).put("ok", true).put("stream", stream.id).put("epoch", stream.epoch).put("token", request.getString("token")).put("rate", stream.rate).put("channels", stream.channels).put("format", stream.format).put("shared_pcm", stream.sharedPcm).put("shared_pcm_owner", "android").put("write_credit", stream.writeCredit).put("presentation_clock", stream.presentationClock).put("verified", false));
			publishInventory();
		}
		catch (Exception failure)
		{
			if (stream != null)
			{
				try
				{
					retire(stream);
				}
				catch (Exception retirement)
				{
					android.util.Log.e("LinuxAudio", "Stream retirement incomplete", retirement);
				}
			}
			try
			{
				send(new JSONObject().put("op", "reply").put("id", request.optLong("id")).put("ok", false).put("error", failure.getMessage()));
			}
			catch (Exception ignored)
			{
			}
			updateWakeLock();
			updatePolicy();
			try
			{
				publishInventory();
			}
			catch (Exception ignored)
			{
			}
			status("Stream setup failed: " + failure.getMessage());
		}
	}

	private void profile(String endpoint, String profile) throws Exception
	{
		if (!profile.equals("stereo") && !profile.equals("headset"))
			throw new IOException("Unknown headset profile");
		JSONObject selected = devices.describe(endpoint);
		if (selected == null)
			throw new IOException("Endpoint unavailable");
		String group = selected.getString("group");
		AudioDeviceInfo communication = devices.communication(group);
		if (profile.equals("headset") && communication == null)
			throw new IOException("Android headset profile unavailable");
		for (StreamIo stream : streams.values())
		{
			JSONObject descriptor = devices.describe(stream.endpoint);
			if (descriptor != null && descriptor.optString("group").equals(group) && stream.headset != profile.equals("headset"))
				retire(stream);
		}
		// Selecting a profile is admission policy, not an active call. Acquire
		// communication routing only when StreamIo prepares a headset stream.
		explicitProfiles.put(group, profile);
		updatePolicy();
		publishInventory();
	}

	private void devicesChanged()
	{
		if (operations == null)
			return;
		operations.post(() -> {
			try
			{
				devices.refresh();
				for (StreamIo stream : streams.values())
				{
					AudioDeviceInfo device = devices.get(stream.endpoint);
					if (device == null || device.getId() != stream.selected.getId())
						stream.close();
				}
				updatePolicy();
				publishInventory();
			}
			catch (Exception failure)
			{
				status("Device discovery failed: " + failure.getMessage());
			}
		});
	}

	void priorityChanged(boolean pause, String reason)
	{
		boolean changed = suspended != pause;
		suspended = pause;
		suspendReason = reason;
		if (pause)
			closeStreams();
		if (changed && operations != null)
		{
			operations.post(() -> {
				try
				{
					if (pause)
						send(new JSONObject().put("op", "suspend").put("reason", reason));
					publishInventory();
				}
				catch (Exception ignored)
				{
				}
			});
		}
	}

	void streamRoute(StreamIo stream, int actual)
	{
		try
		{
			send(new JSONObject().put("op", "route").put("stream", stream.id).put("epoch", stream.epoch).put("endpoint", stream.endpoint).put("actual_android_id", actual).put("verified", true));
		}
		catch (Exception ignored)
		{
		}
		status("Linux audio routed to " + stream.selected.getProductName());
	}

	void streamEnded(StreamIo stream, String failure)
	{
		if (failure == null)
			failure = "Unknown stream failure";
		streams.remove(stream.id, stream);
		routes.release(stream.id);
		updateWakeLock();
		try
		{
			send(new JSONObject().put("op", "stream_closed").put("stream", stream.id).put("epoch", stream.epoch).put("error", failure));
		}
		catch (Exception ignored)
		{
		}
		if (!failure.isEmpty())
		{
			android.util.Log.e("LinuxAudio", "Stream " + stream.id + " stopped: " + failure);
			status("Audio stopped: " + failure);
		}
		if (operations != null)
			operations.post(() -> { updatePolicy(); try { publishInventory(); } catch (Exception ignored) { } });
	}

	private void retire(StreamIo stream) throws Exception
	{
		stream.cancel();
		if (!stream.awaitStopped())
			throw new IOException("Android stream is still releasing its audio path");
		streams.remove(stream.id, stream);
		routes.release(stream.id);
		updateWakeLock();
	}

	private void updatePolicy()
	{
		if (!running || connection == null)
		{
			policy.idle();
			return;
		}
		boolean headsetActive = routes.requiresCommunication();
		if (!headsetActive)
			policy.releaseCommunication();
		if (streams.isEmpty() && !headsetActive && !suspended)
			policy.idle();
	}

	private synchronized void updateWakeLock()
	{
		if (running && !streams.isEmpty())
		{
			if (!wakeLock.isHeld())
				wakeLock.acquire();
		}
		else if (wakeLock.isHeld())
			wakeLock.release();
	}

	private void closeStreams()
	{
		for (StreamIo stream : new ArrayList<>(streams.values()))
			stream.cancel();
	}

	private void send(JSONObject message) throws IOException
	{
		Wire link = connection;
		if (link == null)
			throw new IOException("Broker not connected");
		link.send(message);
	}

	private void publishInventory() throws Exception
	{
		JSONArray inventory = devices.refresh();
		for (int i = 0; i < inventory.length(); ++i)
		{
			JSONObject descriptor = inventory.getJSONObject(i);
			String group = descriptor.getString("group");
			boolean headsetActive = explicitProfiles.getOrDefault(group, "stereo").equals("headset");
			for (StreamIo stream : streams.values())
			{
				JSONObject active = devices.describe(stream.endpoint);
				if (active != null && active.optString("group").equals(group) && stream.headset)
					headsetActive = true;
			}
			String busy = routes.conflict(descriptor.getString("key"), descriptor.getString("resource"));
			String selectedProfile = explicitProfiles.get(group);
			if (selectedProfile != null && (descriptor.getString("profile").equals("headset") || descriptor.getString("profile").equals("stereo")) && !selectedProfile.equals(descriptor.getString("profile")))
				busy = "Bluetooth profile is " + selectedProfile;
			else if (headsetActive && descriptor.getString("profile").equals("stereo"))
				busy = "Bluetooth headset mode is active";
			descriptor.put("busy", busy);
		}
		send(new JSONObject().put("op", "inventory").put("devices", inventory).put("suspended", suspended).put("reason", suspendReason));
	}

	private void status(String text)
	{
		statusText = text;
	}

	@Override
	public void onDestroy()
	{
		running = false;
		closeStreams();
		if (connection != null)
			connection.close();
		if (connectionThread != null)
			connectionThread.interrupt();
		policy.close();
		updateWakeLock();
		devices.manager.unregisterAudioDeviceCallback(deviceCallback);
		operationThread.quitSafely();
		status("Stopped — tap Start to enable Linux audio");
		super.onDestroy();
	}

	@Override
	public IBinder onBind(Intent intent)
	{
		return null;
	}
}
