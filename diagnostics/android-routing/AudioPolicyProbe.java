import android.os.Looper;
import android.content.Context;
import java.lang.reflect.*;
public final class AudioPolicyProbe
{
	public static void main(String[] args) throws Exception
	{
		Looper.prepareMainLooper();
		Class<?> at = Class.forName("android.app.ActivityThread");
		Object thread = at.getMethod("systemMain").invoke(null);
		Context context = (Context)at.getMethod("getSystemContext").invoke(thread);
		System.out.println("uid=" + android.os.Process.myUid() + " routingPermission=" + context.checkSelfPermission("android.permission.MODIFY_AUDIO_ROUTING"));
		for (String name : new String[] {"android.media.audiopolicy.AudioMixingRule", "android.media.audiopolicy.AudioMix$Builder", "android.media.audiopolicy.AudioPolicy$Builder", "android.media.AudioManager"})
		{
			Class<?> cls = Class.forName(name);
			for (Field f : cls.getDeclaredFields())
				if (f.getName().contains("SESSION_ID"))
					System.out.println(f + "=" + f.get(null));
			for (Method m : cls.getDeclaredMethods())
				if (m.getName().matches("setDevice|addMix|registerAudioPolicy|unregisterAudioPolicy|setPreferredDevicesForCapturePreset|setPreferredDevicesForStrategy"))
					System.out.println(m);
		}
	}
}
