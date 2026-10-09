import android.os.Looper;
import android.content.Context;
import android.media.*;
import java.lang.reflect.*;
public final class AudioRouteProbe
{
	static Object mix(AudioDeviceInfo device, int session, AudioFormat format) throws Exception
	{
		Class<?> ruleClass = Class.forName("android.media.audiopolicy.AudioMixingRule"), ruleBuilder = Class.forName("android.media.audiopolicy.AudioMixingRule$Builder");
		Object r = ruleBuilder.getConstructor().newInstance();
		ruleBuilder.getMethod("addMixRule", int.class, Object.class).invoke(r, 16, Integer.valueOf(session));
		Object rule = ruleBuilder.getMethod("build").invoke(r);
		Class<?> mixBuilder = Class.forName("android.media.audiopolicy.AudioMix$Builder");
		Object b = mixBuilder.getConstructor(ruleClass).newInstance(rule);
		mixBuilder.getMethod("setRouteFlags", int.class).invoke(b, 1);
		mixBuilder.getMethod("setDevice", AudioDeviceInfo.class).invoke(b, device);
		mixBuilder.getMethod("setFormat", AudioFormat.class).invoke(b, format);
		return mixBuilder.getMethod("build").invoke(b);
	}
	static boolean separate;
	static AudioTrack track(AudioFormat format, int session, AudioDeviceInfo device)
	{
		AudioTrack t = new AudioTrack.Builder().setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build()).setAudioFormat(format).setPerformanceMode(separate && device != null && device.getType() == 2 ? AudioTrack.PERFORMANCE_MODE_LOW_LATENCY : AudioTrack.PERFORMANCE_MODE_NONE).setSessionId(session).setTransferMode(AudioTrack.MODE_STREAM).setBufferSizeInBytes(48000 * 8 / 5).build();
		if (device != null)
			System.out.println("preferred=" + device.getId() + " accepted=" + t.setPreferredDevice(device));
		return t;
	}
	public static void main(String[] args) throws Exception
	{
		Looper.prepareMainLooper();
		Class<?> at = Class.forName("android.app.ActivityThread");
		Context context = (Context)at.getMethod("getSystemContext").invoke(at.getMethod("systemMain").invoke(null));
		AudioManager am = context.getSystemService(AudioManager.class);
		if (am.getMode() != AudioManager.MODE_NORMAL)
			throw new IllegalStateException("Skip routing probe during a call or communication session");
		AudioDeviceInfo speaker = null, bt = null;
		for (AudioDeviceInfo d : am.getDevices(AudioManager.GET_DEVICES_OUTPUTS))
		{
			if (d.getType() == 2)
				speaker = d;
			if (d.getType() == 8)
				bt = d;
		}
		if (speaker == null || bt == null)
			throw new Exception("Speaker or A2DP missing");
		boolean privileged = args.length > 0 && args[0].equals("policy");
		separate = args.length > 0 && args[0].equals("separate");
		AudioFormat format = new AudioFormat.Builder().setSampleRate(48000).setEncoding(AudioFormat.ENCODING_PCM_FLOAT).setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build();
		int s1 = am.generateAudioSessionId(), s2 = am.generateAudioSessionId();
		Object policy = null;
		AudioTrack t1 = null, t2 = null;
		try
		{
			if (privileged)
			{
				Class<?> pb = Class.forName("android.media.audiopolicy.AudioPolicy$Builder"), pc = Class.forName("android.media.audiopolicy.AudioPolicy"), mc = Class.forName("android.media.audiopolicy.AudioMix");
				Object builder = pb.getConstructor(Context.class).newInstance(context);
				pb.getMethod("addMix", mc).invoke(builder, mix(speaker, s1, format));
				pb.getMethod("addMix", mc).invoke(builder, mix(bt, s2, format));
				policy = pb.getMethod("build").invoke(builder);
				int status = (Integer)AudioManager.class.getMethod("registerAudioPolicy", pc).invoke(am, policy);
				System.out.println("register=" + status);
				if (status != 0)
					return;
			}
			t1 = track(format, s1, privileged ? null : speaker);
			t2 = track(format, s2, privileged ? null : bt);
			float[] zero = new float[4800];
			t1.play();
			t2.play();
			for (int i = 0; i < 80; i++)
			{
				int w1 = t1.write(zero, 0, zero.length, AudioTrack.WRITE_NON_BLOCKING), w2 = t2.write(zero, 0, zero.length, AudioTrack.WRITE_NON_BLOCKING);
				if (i % 10 == 0)
					System.out.println("policy=" + privileged + " step=" + i + " expected=" + speaker.getId() + "," + bt.getId() + " routed=" + (t1.getRoutedDevice() == null ? 0 : t1.getRoutedDevice().getId()) + "," + (t2.getRoutedDevice() == null ? 0 : t2.getRoutedDevice().getId()) + " head=" + t1.getPlaybackHeadPosition() + "," + t2.getPlaybackHeadPosition() + " write=" + w1 + "," + w2);
				Thread.sleep(25);
			}
		}
		finally
		{
			if (t1 != null)
				t1.release();
			if (t2 != null)
				t2.release();
			if (policy != null)
				AudioManager.class.getMethod("unregisterAudioPolicy", Class.forName("android.media.audiopolicy.AudioPolicy")).invoke(am, policy);
			System.out.println("released tracks and policy");
		}
		System.exit(0);
	}
}
