package dev.kiraly.linuxaudio;

/**
 * Control-only JNI facade. PCM never enters a Java array or ByteBuffer.
 */
final class NativeAudio
{
	static
	{
		System.loadLibrary("linux_audio");
	}
	static native long create(int device, int rate, int channels, int sampleBytes, boolean capture, int source, boolean headset, long epoch, boolean presentation, boolean preferMmap, String packageName) throws java.io.IOException;
	static native void run(long handle, int descriptor, StreamIo owner) throws java.io.IOException;
	static native void cancel(long handle);
	static native void destroy(long handle);
}
