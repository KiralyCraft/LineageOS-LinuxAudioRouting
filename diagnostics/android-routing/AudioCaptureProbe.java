import android.os.Looper;
import android.content.Context;
import android.media.*;
public final class AudioCaptureProbe
{
	public static void main(String[] args) throws Exception
	{
		Looper.prepareMainLooper();
		Class<?> at = Class.forName("android.app.ActivityThread");
		Context c = (Context)at.getMethod("getSystemContext").invoke(at.getMethod("systemMain").invoke(null));
		AudioManager am = c.getSystemService(AudioManager.class);
		if (am.getMode() != AudioManager.MODE_NORMAL)
			throw new Exception("Non-normal audio mode; skip capture probe");
		System.out.println("uid=" + android.os.Process.myUid() + " captureOutputPermission=" + c.checkSelfPermission("android.permission.CAPTURE_AUDIO_OUTPUT"));
		boolean headset = args.length > 0 && args[0].equals("phone-headset");
		boolean communication = false;
		AudioRecord[] recorders = new AudioRecord[2];
		long[] frames = new long[2], nonzero = new long[2];
		float[] data = new float[4800];
		try
		{
			if (headset)
			{
				for (AudioDeviceInfo d : am.getAvailableCommunicationDevices())
					if (d.getType() == AudioDeviceInfo.TYPE_BLUETOOTH_SCO)
					{
						am.setMode(AudioManager.MODE_IN_COMMUNICATION);
						communication = true;
						if (!am.setCommunicationDevice(d))
							throw new Exception("Communication route rejected");
						break;
					}
				if (!communication)
					throw new Exception("No Bluetooth headset connected");
			}
			int i = 0;
			for (AudioDeviceInfo d : am.getDevices(AudioManager.GET_DEVICES_INPUTS))
				if (i < 2 && ((!headset && d.getType() == AudioDeviceInfo.TYPE_BUILTIN_MIC) || (headset && ((i == 0 && d.getType() == AudioDeviceInfo.TYPE_BUILTIN_MIC) || d.getType() == AudioDeviceInfo.TYPE_BLUETOOTH_SCO))))
				{
					int source = d.getType() == AudioDeviceInfo.TYPE_BLUETOOTH_SCO ? MediaRecorder.AudioSource.VOICE_COMMUNICATION : MediaRecorder.AudioSource.UNPROCESSED;
					recorders[i] = new AudioRecord.Builder().setAudioSource(source).setAudioFormat(new AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_FLOAT).setSampleRate(48000).setChannelMask(AudioFormat.CHANNEL_IN_MONO).build()).setBufferSizeInBytes(19200).build();
					System.out.println("index=" + i + " wanted=" + d.getId() + " preferred=" + recorders[i].setPreferredDevice(d));
					recorders[i].startRecording();
					i++;
				}
			for (int step = 0; step < 80; step++)
			{
				for (i = 0; i < 2; i++)
					if (recorders[i] != null)
					{
						int n = recorders[i].read(data, 0, data.length, AudioRecord.READ_NON_BLOCKING);
						if (n < 0)
							throw new Exception("read=" + n);
						frames[i] += n;
						for (int j = 0; j < n; j++)
							if (data[j] != 0)
								nonzero[i]++;
						if (step % 20 == 0)
						{
							AudioRecordingConfiguration cfg = recorders[i].getActiveRecordingConfiguration();
							AudioDeviceInfo route = recorders[i].getRoutedDevice();
							System.out.println("index=" + i + " step=" + step + " routed=" + (route == null ? 0 : route.getId()) + " silenced=" + (cfg == null ? "unknown" : cfg.isClientSilenced()) + " frames=" + frames[i] + " nonzero=" + nonzero[i]);
						}
					}
				Thread.sleep(25);
			}
		}
		finally
		{
			for (AudioRecord r : recorders)
				if (r != null)
					r.release();
			if (communication)
			{
				am.clearCommunicationDevice();
				am.setMode(AudioManager.MODE_NORMAL);
			}
			System.out.println("Released recorders; no PCM stored");
		}
		System.exit(0);
	}
}
