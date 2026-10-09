package dev.kiraly.linuxaudio;

/** Frame-based buffer policy; no pacing timers, sample rewriting or catch-up drops. */
final class PlaybackBuffer
{
	private int capacity;
	private int size;
	private int underruns;

	PlaybackBuffer(int capacity, int packetFrames, int burstFrames, int underruns)
	{
		this.capacity = Math.max(1, capacity);
		this.size = (int)Math.min(this.capacity, Math.max(2L * Math.max(1, packetFrames), 2L * Math.max(1, burstFrames)));
		this.underruns = underruns;
	}

	int size()
	{
		return size;
	}

	void applied(int actual, int capacity)
	{
		if (actual > 0 && capacity >= actual)
		{
			this.capacity = capacity;
			this.size = actual;
		}
	}

	boolean observe(int count)
	{
		boolean changed = count > underruns && size < capacity;
		underruns = count;
		if (changed)
			size = (int)Math.min(capacity, (long)size * 2);
		return changed;
	}
}
