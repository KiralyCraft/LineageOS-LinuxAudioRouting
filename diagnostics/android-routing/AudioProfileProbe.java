package dev.kiraly.linuxaudio;
import android.content.Context;
import android.media.*;
import android.os.Looper;
import java.nio.ByteBuffer;

/**
 * Silence-only, bounded track sizing probe. Does not change Android volume.
 */
public final class AudioProfileProbe
{
	public static void main(String[] args) throws Exception
	{
		Looper.prepareMainLooper();
		Class<?> activityThread = Class.forName("android.app.ActivityThread");
		Context context = (Context)activityThread.getMethod("getSystemContext").invoke(activityThread.getMethod("systemMain").invoke(null));
		AudioManager manager = context.getSystemService(AudioManager.class);
		if (manager.getMode() != AudioManager.MODE_NORMAL)
			throw new IllegalStateException("Probe requires idle communication policy");
		int type = Integer.parseInt(args[0]);
		boolean generic = true;
		AudioDeviceInfo selected = null;
		for (AudioDeviceInfo device : manager.getDevices(AudioManager.GET_DEVICES_OUTPUTS))
			if (device.getType() == type)
				selected = device;
		if (selected == null)
			throw new IllegalStateException("Requested output absent");
		boolean headset = type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO;
		int rate = headset ? 16000 : 48000;
		int frameBytes = headset ? 2 : 8;
		int encoding = headset ? AudioFormat.ENCODING_PCM_16BIT : AudioFormat.ENCODING_PCM_FLOAT;
		int mask = headset ? AudioFormat.CHANNEL_OUT_MONO : AudioFormat.CHANNEL_OUT_STEREO;
		AudioTrack track = null;
		try
		{
			if (headset)
			{
				manager.setMode(AudioManager.MODE_IN_COMMUNICATION);
				if (!manager.setCommunicationDevice(selected))
					throw new IllegalStateException("Communication route rejected");
			}
			int minimum = AudioTrack.getMinBufferSize(rate, mask, encoding);
			AudioTrack.Builder builder = new AudioTrack.Builder().setAudioFormat(new AudioFormat.Builder().setSampleRate(rate).setChannelMask(mask).setEncoding(encoding).build()).setAudioAttributes(new AudioAttributes.Builder().setUsage(headset ? AudioAttributes.USAGE_VOICE_COMMUNICATION : AudioAttributes.USAGE_MEDIA).setContentType(headset ? AudioAttributes.CONTENT_TYPE_SPEECH : AudioAttributes.CONTENT_TYPE_MUSIC).build()).setTransferMode(AudioTrack.MODE_STREAM).setPerformanceMode(Integer.parseInt(args[2]));
			// Omitting the size lets AudioFlinger apply the selected output's
			// minimum instead of the generic pre-route estimate.
			if (generic)
				builder.setBufferSizeInBytes(minimum);
			if (args[1].equals("burst"))
			{
				int burst = Integer.parseInt(manager.getProperty(AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER));
				int nativeRate = Integer.parseInt(manager.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE));
				int frames = (int)((2L * burst * rate + nativeRate - 1) / nativeRate);
				builder.setBufferSizeInBytes(frames * frameBytes);
				System.out.println("burst_frames=" + burst + " native_rate=" + nativeRate + " requested_frames=" + frames);
			}
			track = builder.build();
			int nativeRate = Integer.parseInt(manager.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE));
			int nativeBurst = Integer.parseInt(manager.getProperty(AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER));
			int burst = (int)(((long)nativeBurst * rate + nativeRate - 1) / nativeRate);
			PlaybackBuffer policy = new PlaybackBuffer(track.getBufferCapacityInFrames(), rate / 100, burst, track.getUnderrunCount());
			policy.applied(track.setBufferSizeInFrames(policy.size()), track.getBufferCapacityInFrames());
			track.setStartThresholdInFrames(1);
			if (!track.setPreferredDevice(selected))
				throw new IllegalStateException("Preferred route rejected");
			System.out.println("create type=" + type + " generic=" + generic + " generic_frames=" + minimum / frameBytes + " capacity=" + track.getBufferCapacityInFrames() + " size=" + track.getBufferSizeInFrames());
			track.play();
			ByteBuffer silence = ByteBuffer.allocateDirect(rate / 100 * frameBytes);
			AudioTimestamp timestamp = new AudioTimestamp();
			long sent = 0, nextReport = 0, deadline = System.nanoTime() + 4_000_000_000L;
			while (System.nanoTime() < deadline)
			{
				silence.clear();
				int written = track.write(silence, silence.remaining(), AudioTrack.WRITE_BLOCKING);
				if (written <= 0)
					throw new IllegalStateException("Write failed: " + written);
				sent += written / frameBytes;
				if (policy.observe(track.getUnderrunCount()) || track.getBufferSizeInFrames() != policy.size())
					policy.applied(track.setBufferSizeInFrames(policy.size()), track.getBufferCapacityInFrames());
				if (sent >= nextReport)
				{
					nextReport = sent + rate / 2;
					AudioDeviceInfo actual = track.getRoutedDevice();
					boolean valid = track.getTimestamp(timestamp);
					System.out.println("sample expected=" + selected.getId() + " actual=" + (actual == null ? 0 : actual.getId()) + " rate=" + rate + " sent=" + sent + " head=" + Integer.toUnsignedLong(track.getPlaybackHeadPosition()) + " capacity=" + track.getBufferCapacityInFrames() + " size=" + track.getBufferSizeInFrames() + " underruns=" + track.getUnderrunCount() + " timestamp_valid=" + valid + " timestamp_frame=" + timestamp.framePosition + " timestamp_ns=" + timestamp.nanoTime + " now_ns=" + System.nanoTime());
				}
			}
		}
		finally
		{
			if (track != null)
				track.release();
			if (headset)
			{
				manager.clearCommunicationDevice();
				manager.setMode(AudioManager.MODE_NORMAL);
			}
		}
		System.exit(0);
	}
}
