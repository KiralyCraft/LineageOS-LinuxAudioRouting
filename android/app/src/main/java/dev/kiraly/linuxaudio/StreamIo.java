package dev.kiraly.linuxaudio;

import android.media.AudioAttributes;
import android.media.AudioManager;
import android.media.AudioDeviceInfo;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.AudioTimestamp;
import android.media.AudioTrack;
import android.media.MediaRecorder;
import android.os.Handler;
import android.os.HandlerThread;
import org.json.JSONObject;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

final class StreamIo implements AutoCloseable, Runnable
{
	final AudioService service;
	final String endpoint;
	final long id;
	final long epoch;
	final AudioDeviceInfo selected;
	final boolean capture;
	final int captureSource;
	private volatile boolean cancelled;
	final boolean headset;
	final boolean writeCredit;
	final boolean sharedPcm;
	final boolean preferMmap;
	private long nativeHandle;
	final boolean presentationClock;
	private PlaybackBuffer playbackBuffer;
	private final AudioTimestamp presentation = new AudioTimestamp();
	private long lastPresentationTime;
	private long lastPresentationFrame;
	private long nextPresentationQuery;
	final int rate;
	final int channels;
	final int sampleBytes;
	final int frameBytes;
	final String format;
	private final CountDownLatch activated = new CountDownLatch(1);
	private volatile boolean live = true;
	private volatile boolean verified;
	private volatile String routeFailure = "";
	private Wire wire;
	private AudioTrack track;
	private AudioRecord recorder;
	private Thread worker;
	private HandlerThread clockThread;
	private volatile long sentFrames;
	private long playedHigh;
	private long playedLast;
	private long probedFrames;
	private final Object resources = new Object();
	private boolean released;
	private final android.media.AudioRouting.OnRoutingChangedListener routeListener = routing -> routeChanged();

	private void routeChanged()
	{
		if (verified && live)
		{
			try
			{
				ensureRoute();
			}
			catch (IOException failure)
			{
				if (live && !service.suspended)
					routeFailure = failure.getMessage();
				close();
			}
		}
	}
	private static final byte[] EMPTY = new byte[0];

	StreamIo(AudioService service, JSONObject request, AudioDeviceInfo selected, JSONObject device) throws Exception
	{
		this.service = service;
		this.selected = selected;
		endpoint = request.getString("endpoint");
		id = request.getLong("stream");
		epoch = request.getLong("epoch");
		capture = device.getString("direction").equals("input");
		captureSource = device.optInt("capture_source", MediaRecorder.AudioSource.UNPROCESSED);
		writeCredit = !capture && request.optBoolean("write_credit", false);
		preferMmap = request.optBoolean("prefer_mmap", true);
		sharedPcm = request.optBoolean("shared_pcm", false) && request.optString("shared_pcm_owner").equals("android");
		// MMAP Bluetooth timestamps have not been calibrated to acoustic output.
		presentationClock = !capture && request.optBoolean("presentation_clock", false) && (!sharedPcm || (selected.getType() != AudioDeviceInfo.TYPE_BLUETOOTH_A2DP && selected.getType() != AudioDeviceInfo.TYPE_BLUETOOTH_SCO));
		headset = device.getString("profile").equals("headset");
		rate = request.getInt("rate");
		channels = request.getInt("channels");
		format = request.getString("format");
		sampleBytes = format.equals("f32le") ? 4 : 2;
		frameBytes = sampleBytes * channels;
		if (rate < 8000 || rate > 96000 || channels < 1 || channels > 2 || (!format.equals("f32le") && !format.equals("s16le")))
			throw new IOException("Unsupported PCM specification");
	}

	void prepare(JSONObject request) throws Exception
	{
		int encoding = sampleBytes == 4 ? AudioFormat.ENCODING_PCM_FLOAT : AudioFormat.ENCODING_PCM_16BIT;
		int mask = capture ? AudioFormat.CHANNEL_IN_MONO : AudioFormat.CHANNEL_OUT_MONO;
		if (channels == 2)
			mask = capture ? AudioFormat.CHANNEL_IN_STEREO : AudioFormat.CHANNEL_OUT_STEREO;
		AudioFormat audioFormat = new AudioFormat.Builder().setSampleRate(rate).setEncoding(encoding).setChannelMask(mask).build();
		if (headset)
		{
			AudioDeviceInfo communicationDevice = selected;
			if (capture)
			{
				communicationDevice = null;
				for (AudioDeviceInfo candidate : service.devices.manager.getAvailableCommunicationDevices())
					if (candidate.getType() == AudioDeviceInfo.TYPE_BLUETOOTH_SCO && candidate.getAddress().equals(selected.getAddress()))
						communicationDevice = candidate;
			}
			if (communicationDevice == null || !service.policy.headset(communicationDevice))
				throw new IOException("Android communication routing unavailable");
		}
		synchronized (resources)
		{
			if (!live)
				throw new IOException("Stream was cancelled during preparation");
			if (sharedPcm)
			{
				nativeHandle = NativeAudio.create(selected.getId(), rate, channels, sampleBytes, capture, captureSource, headset, epoch, presentationClock, preferMmap, service.getPackageName());
			}
			else if (capture)
			{
				int minimum = AudioRecord.getMinBufferSize(rate, mask, encoding);
				if (minimum <= 0)
					throw new IOException("AudioRecord rejected the PCM format");
				recorder = new AudioRecord.Builder().setAudioSource(captureSource).setAudioFormat(audioFormat).setBufferSizeInBytes(Math.max(minimum, rate / 25 * frameBytes)).build();
				if (!recorder.setPreferredDevice(selected))
					throw new IOException("Microphone route rejected");
			}
			else
			{
				int minimum = AudioTrack.getMinBufferSize(rate, mask, encoding);
				if (minimum <= 0)
					throw new IOException("AudioTrack rejected the PCM format");
				// Local playback and Bluetooth media must not compete for the
				// same mixed output. Bluetooth has its own buffered transport;
				// explicitly select that media profile and verify the actual route.
				int performanceMode = AudioTrack.PERFORMANCE_MODE_LOW_LATENCY;
				if (selected.getType() == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP)
					performanceMode = AudioTrack.PERFORMANCE_MODE_POWER_SAVING;
				AudioAttributes attributes = new AudioAttributes.Builder().setUsage(headset ? AudioAttributes.USAGE_VOICE_COMMUNICATION : AudioAttributes.USAGE_MEDIA).setContentType(headset ? AudioAttributes.CONTENT_TYPE_SPEECH : AudioAttributes.CONTENT_TYPE_MUSIC).build();
				track = new AudioTrack.Builder().setAudioAttributes(attributes).setAudioFormat(audioFormat).setTransferMode(AudioTrack.MODE_STREAM).setPerformanceMode(performanceMode).setBufferSizeInBytes(Math.max(minimum, rate / 50 * frameBytes)).build();
				int burst = 1;
				try
				{
					int nativeRate = Integer.parseInt(service.devices.manager.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE));
					int nativeBurst = Integer.parseInt(service.devices.manager.getProperty(AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER));
					if (nativeRate > 0 && nativeBurst > 0 && nativeBurst <= nativeRate)
						burst = (int)(((long)nativeBurst * rate + nativeRate - 1) / nativeRate);
				}
				catch (NumberFormatException ignored)
				{
				}
				playbackBuffer = new PlaybackBuffer(track.getBufferCapacityInFrames(), Math.max(1, rate / 100), burst, track.getUnderrunCount());
				applyPlaybackBuffer();
				track.setStartThresholdInFrames(1);
				if (!track.setPreferredDevice(selected))
					throw new IOException("Playback route rejected");
			}
			if (sharedPcm)
			{
				// AAudio reports disconnection through its native error callback.
			}
			else if (capture)
				recorder.addOnRoutingChangedListener(routeListener, new Handler(android.os.Looper.getMainLooper()));
			else
				track.addOnRoutingChangedListener(routeListener, new Handler(android.os.Looper.getMainLooper()));
		}
		Wire attached;
		try (Wire authentication = new Wire())
		{
			attached = authentication.attach(id, request.getString("token"));
		}
		synchronized (resources)
		{
			if (!live)
			{
				attached.close();
				throw new IOException("Stream was cancelled during attachment");
			}
			wire = attached;
			worker = new Thread(this, "linux-audio-" + id);
			worker.start();
		}
	}

	private void applyPlaybackBuffer() throws IOException
	{
		int actual = track.setBufferSizeInFrames(playbackBuffer.size());
		if (actual <= 0)
			throw new IOException("AudioTrack buffer adjustment rejected: " + actual);
		playbackBuffer.applied(actual, track.getBufferCapacityInFrames());
	}

	private boolean observePlaybackBuffer() throws IOException
	{
		// Only called by the writing thread, after Android has accepted data.
		boolean changed = playbackBuffer.observe(track.getUnderrunCount());
		// Android may recreate/enlarge the track when preferred routing takes
		// effect. Reapply the effective limit after that platform transition.
		if (changed || track.getBufferSizeInFrames() != playbackBuffer.size())
		{
			applyPlaybackBuffer();
			return true;
		}
		return false;
	}

	void nativeReady(int device, boolean mmap) throws Exception
	{
		if (device != selected.getId() || !live)
			throw new IOException("Native audio route lost");
		verified = true;
		android.util.Log.i("LinuxAudio", "Shared PCM AAudio stream=" + id + " device=" + device + " prefer_mmap=" + preferMmap + " mmap=" + mmap);
		service.streamRoute(this, device);
	}

	void activate()
	{
		activated.countDown();
	}

	private void verifyRoute() throws Exception
	{
		// Failure bound only. Successful readiness is driven by route and
		// consumed-frame progress, never by sleeping for a guessed delay.
		long deadline = System.nanoTime() + 5_000_000_000L;
		long stableFrom = 0;
		ByteBuffer probe = ByteBuffer.allocateDirect(Math.max(1, rate / 100) * frameBytes).order(ByteOrder.LITTLE_ENDIAN);
		if (capture)
			recorder.startRecording();
		else
			track.play();
		while (live && !service.suspended && System.nanoTime() < deadline)
		{
			probe.clear();
			int result;
			if (capture)
				result = recorder.read(probe, probe.capacity(), AudioRecord.READ_BLOCKING);
			else
				result = track.write(probe, probe.capacity(), AudioTrack.WRITE_BLOCKING);
			if (result > 0)
				probedFrames += result / frameBytes;
			if (result < 0)
				throw new IOException("Android route probe failed: " + result);
			AudioDeviceInfo routed = capture ? recorder.getRoutedDevice() : track.getRoutedDevice();
			if (routed != null && routed.getId() == selected.getId())
			{
				if (!capture)
				{
					if (observePlaybackBuffer())
						stableFrom = probedFrames;
					if (probedFrames - stableFrom < 4L * playbackBuffer.size() || track.getPlaybackHeadPosition() == 0)
						continue;
				}
				verified = true;
				// Keep the verified route running; exclude probe frames from
				// the Linux timeline rather than invalidating it with a flush.
				service.streamRoute(this, routed.getId());
				return;
			}
		}
		throw new IOException("Android did not route this stream to the selected device");
	}

	private void ensureRoute() throws IOException
	{
		AudioDeviceInfo routed = capture ? recorder.getRoutedDevice() : track.getRoutedDevice();
		if (!live || service.suspended || routed == null || routed.getId() != selected.getId())
			throw new IOException("Selected route lost");
	}

	private synchronized void clock() throws IOException
	{
		long head = Integer.toUnsignedLong(track.getPlaybackHeadPosition());
		if (head < playedLast)
			playedHigh += 1L << 32;
		playedLast = head;
		long time = System.nanoTime();
		// A cached AudioTimestamp can stop advancing during an underrun.
		// Sample the monotonic playback-head counter for drain/progress.
		long frame = Math.max(0, playedHigh + head - probedFrames);
		wire.sendPcm(Wire.CLOCK, epoch, Math.min(frame, sentFrames), time, 0, EMPTY, 0);
		if (presentationClock && frame >= nextPresentationQuery)
		{
			// Query on audio progress, at most four times per audio second.
			// Repeated clock notifications must not become Binder/API polling.
			nextPresentationQuery = frame + Math.max(1, rate / 4);
			if (!track.getTimestamp(presentation) || presentation.framePosition < probedFrames)
				return;
			long position = presentation.framePosition - probedFrames;
			// Retain the platform frame/time pair exactly. Its frame position
			// is a different timeline observation from playback-head progress.
			if (position <= sentFrames && position >= lastPresentationFrame && presentation.nanoTime > lastPresentationTime && presentation.nanoTime <= time + 2_000_000_000L)
			{
				lastPresentationFrame = position;
				lastPresentationTime = presentation.nanoTime;
				wire.sendPcm(Wire.PRESENTATION, epoch, position, presentation.nanoTime, 0, EMPTY, 0);
			}
		}
	}

	private void notifyPlaybackClock()
	{
		if (!live)
			return;
		try
		{
			ensureRoute();
			clock();
		}
		catch (Exception failure)
		{
			if (live && !service.suspended)
				routeFailure = failure.getMessage();
			close();
		}
	}

	private void playback() throws Exception
	{
		clockThread = new HandlerThread("linux-audio-clock");
		clockThread.start();
		track.setPositionNotificationPeriod(Math.max(1, rate / 200));
		track.setPlaybackPositionUpdateListener(new AudioTrack.OnPlaybackPositionUpdateListener() {
			public void onMarkerReached(AudioTrack ignored)
			{
				notifyPlaybackClock();
			}
			public void onPeriodicNotification(AudioTrack ignored)
			{
				notifyPlaybackClock();
			}
		}, new Handler(clockThread.getLooper()));
		ByteBuffer samples = ByteBuffer.allocateDirect(Math.max(1, rate / 10) * frameBytes).order(ByteOrder.LITTLE_ENDIAN);
		long nextFrame = 0;
		Wire.Packet packet = new Wire.Packet();
		while (live)
		{
			wire.receivePcm(packet);
			if (packet.kind != Wire.AUDIO || packet.epoch != epoch || packet.frame != nextFrame || packet.frames < 1 || packet.frames > rate / 10 || packet.length != packet.frames * frameBytes)
				throw new IOException("PCM discontinuity or invalid frame");
			ensureRoute();
			samples.clear();
			samples.put(packet.data, 0, packet.length).flip();
			while (samples.hasRemaining() && live)
			{
				int written = track.write(samples, samples.remaining(), AudioTrack.WRITE_BLOCKING);
				if (written <= 0 || written % frameBytes != 0)
					throw new IOException("AudioTrack write failed");
				sentFrames += written / frameBytes;
				observePlaybackBuffer();
				if (writeCredit)
					wire.sendPcm(Wire.CREDIT, epoch, sentFrames, System.nanoTime(), 0, EMPTY, 0);
			}
			long marker = probedFrames + sentFrames;
			if (marker <= Integer.MAX_VALUE)
				track.setNotificationMarkerPosition((int)marker);
			nextFrame += packet.frames;
			clock();
		}
	}

	private void capture() throws Exception
	{
		ByteBuffer samples = ByteBuffer.allocateDirect(Math.max(1, rate / 100) * frameBytes).order(ByteOrder.LITTLE_ENDIAN);
		byte[] bytes = new byte[samples.capacity()];
		long nextFrame = 0;
		AudioTimestamp timestamp = new AudioTimestamp();
		while (live)
		{
			samples.clear();
			int read = recorder.read(samples, samples.capacity(), AudioRecord.READ_BLOCKING);
			if (read <= 0 || read % frameBytes != 0)
				throw new IOException("AudioRecord read failed");
			ensureRoute();
			samples.position(0);
			samples.get(bytes, 0, read);
			long time = System.nanoTime();
			if (recorder.getTimestamp(timestamp, AudioTimestamp.TIMEBASE_MONOTONIC) == AudioRecord.SUCCESS)
				time = timestamp.nanoTime + (probedFrames + nextFrame - timestamp.framePosition) * 1_000_000_000L / rate;
			int frames = read / frameBytes;
			wire.sendPcm(Wire.AUDIO, epoch, nextFrame, time, frames, bytes, read);
			nextFrame += frames;
		}
	}

	@Override
	public void run()
	{
		String failure = "";
		try
		{
			if (!activated.await(5, TimeUnit.SECONDS))
				throw new IOException("Linux PCM attachment timed out");
			if (!live || service.suspended)
				return;
			if (sharedPcm)
			{
				android.os.ParcelFileDescriptor nativeSocket;
				synchronized (resources)
				{
					if (!live)
						return;
					nativeSocket = wire.duplicateNativeDescriptor();
				}
				try (nativeSocket)
				{
					NativeAudio.run(nativeHandle, nativeSocket.getFd(), this);
				}
				return;
			}
			verifyRoute();
			if (capture)
				capture();
			else
				playback();
		}
		catch (Exception error)
		{
			if (live)
				failure = error.getMessage();
		}
		finally
		{
			close();
			if (clockThread != null)
				clockThread.quitSafely();
			synchronized (resources)
			{
				release();
			}
			if (failure.isEmpty() && routeFailure != null)
				failure = routeFailure;
			if (cancelled)
				failure = "";
			service.streamEnded(this, failure);
		}
	}

	private void release()
	{
		if (released)
			return;
		released = true;
		if (nativeHandle != 0)
		{
			NativeAudio.destroy(nativeHandle);
			nativeHandle = 0;
		}
		if (track != null)
		{
			track.removeOnRoutingChangedListener(routeListener);
			track.release();
		}
		if (recorder != null)
		{
			recorder.removeOnRoutingChangedListener(routeListener);
			recorder.release();
		}
	}

	void cancel()
	{
		cancelled = true;
		close();
	}

	boolean awaitStopped() throws InterruptedException
	{
		Thread active = worker;
		if (active != null && active != Thread.currentThread())
		{
			active.join(2000);
			return !active.isAlive();
		}
		return true;
	}

	@Override
	public void close()
	{
		synchronized (resources)
		{
			live = false;
			if (nativeHandle != 0)
				NativeAudio.cancel(nativeHandle);
			activated.countDown();
			if (wire != null)
				wire.close();
			try
			{
				if (track != null && !released)
				{
					track.setVolume(0);
					track.pause();
					track.flush();
				}
			}
			catch (RuntimeException ignored)
			{
			}
			try
			{
				if (recorder != null && !released)
					recorder.stop();
			}
			catch (RuntimeException ignored)
			{
			}
			if (worker == null)
				release();
		}
	}
}
