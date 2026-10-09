package dev.kiraly.linuxaudio;

import android.media.AudioAttributes;
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
	final boolean headset;
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
	private long playbackOrigin;
	private final Object resources = new Object();
	private boolean released;
	private final android.media.AudioRouting.OnRoutingChangedListener routeListener = routing -> routeChanged();

	private void routeChanged()
	{
		if (verified)
		{
			try
			{
				ensureRoute();
			}
			catch (IOException failure)
			{
				if (!service.suspended)
					routeFailure = failure.getMessage();
				close();
			}
		}
	}
	private final AudioTimestamp playbackTimestamp = new AudioTimestamp();
	private static final byte[] EMPTY = new byte[0];

	StreamIo(AudioService service, JSONObject request, AudioDeviceInfo selected, JSONObject device) throws Exception
	{
		this.service = service;
		this.selected = selected;
		endpoint = request.getString("endpoint");
		id = request.getLong("stream");
		epoch = request.getLong("epoch");
		capture = device.getString("direction").equals("input");
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
			if (capture)
			{
				int minimum = AudioRecord.getMinBufferSize(rate, mask, encoding);
				if (minimum <= 0)
					throw new IOException("AudioRecord rejected the PCM format");
				recorder = new AudioRecord.Builder().setAudioSource("true".equals(service.devices.manager.getProperty(android.media.AudioManager.PROPERTY_SUPPORT_AUDIO_SOURCE_UNPROCESSED)) ? MediaRecorder.AudioSource.UNPROCESSED : MediaRecorder.AudioSource.VOICE_RECOGNITION).setAudioFormat(audioFormat).setBufferSizeInBytes(Math.max(minimum, rate / 25 * frameBytes)).build();
				if (!recorder.setPreferredDevice(selected))
					throw new IOException("Microphone route rejected");
			}
			else
			{
				int minimum = AudioTrack.getMinBufferSize(rate, mask, encoding);
				if (minimum <= 0)
					throw new IOException("AudioTrack rejected the PCM format");
				AudioAttributes attributes = new AudioAttributes.Builder().setUsage(headset ? AudioAttributes.USAGE_VOICE_COMMUNICATION : AudioAttributes.USAGE_MEDIA).setContentType(headset ? AudioAttributes.CONTENT_TYPE_SPEECH : AudioAttributes.CONTENT_TYPE_MUSIC).build();
				track = new AudioTrack.Builder().setAudioAttributes(attributes).setAudioFormat(audioFormat).setTransferMode(AudioTrack.MODE_STREAM).setBufferSizeInBytes(Math.max(minimum, rate / 50 * frameBytes)).build();
				track.setStartThresholdInFrames(1);
				if (!track.setPreferredDevice(selected))
					throw new IOException("Playback route rejected");
			}
			if (capture)
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

	void activate()
	{
		activated.countDown();
	}

	private void verifyRoute() throws Exception
	{
		long deadline = System.nanoTime() + 700_000_000L;
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
			if (capture && result > 0)
				probedFrames += result / frameBytes;
			if (result < 0)
				throw new IOException("Android route probe failed: " + result);
			AudioDeviceInfo routed = capture ? recorder.getRoutedDevice() : track.getRoutedDevice();
			if (routed != null && routed.getId() == selected.getId())
			{
				verified = true;
				if (!capture)
				{
					track.pause();
					track.flush();
					playbackOrigin = System.nanoTime();
					track.play();
				}
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
		AudioTimestamp timestamp = playbackTimestamp;
		long time = System.nanoTime();
		long frame = playedHigh + head;
		if (track.getTimestamp(timestamp) && timestamp.nanoTime >= playbackOrigin && timestamp.framePosition <= frame)
		{
			frame = timestamp.framePosition;
			time = timestamp.nanoTime;
		}
		wire.sendPcm(Wire.CLOCK, epoch, Math.min(frame, sentFrames), time, 0, EMPTY, 0);
	}

	private void playback() throws Exception
	{
		clockThread = new HandlerThread("linux-audio-clock");
		clockThread.start();
		track.setPositionNotificationPeriod(Math.max(1, rate / 200));
		track.setPlaybackPositionUpdateListener(new AudioTrack.OnPlaybackPositionUpdateListener() {
			public void onMarkerReached(AudioTrack ignored)
			{
			}
			public void onPeriodicNotification(AudioTrack ignored)
			{
				try
				{
					ensureRoute();
					clock();
				}
				catch (Exception failure)
				{
					if (!service.suspended)
						routeFailure = failure.getMessage();
					close();
				}
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
			}
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
			service.streamEnded(this, failure);
		}
	}

	private void release()
	{
		if (released)
			return;
		released = true;
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

	@Override
	public void close()
	{
		synchronized (resources)
		{
			live = false;
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
