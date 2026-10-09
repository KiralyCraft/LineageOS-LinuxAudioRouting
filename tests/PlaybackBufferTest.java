package dev.kiraly.linuxaudio;

public final class PlaybackBufferTest
{
	private static void require(boolean condition)
	{
		if (!condition)
			throw new AssertionError("Playback buffer policy");
	}

	public static void main(String[] args)
	{
		PlaybackBuffer policy = new PlaybackBuffer(15376, 480, 144, 0);
		require(policy.size() == 960);
		require(!policy.observe(0));
		require(policy.observe(1) && policy.size() == 1920);
		require(!policy.observe(1));
		policy.applied(2048, 15376);
		require(policy.observe(2) && policy.size() == 4096);
		policy.applied(15376, 15376);
		require(!policy.observe(3) && policy.size() == 15376);
		PlaybackBuffer small = new PlaybackBuffer(192, 480, 144, 5);
		require(small.size() == 192 && !small.observe(6));
		PlaybackBuffer large = new PlaybackBuffer(Integer.MAX_VALUE, Integer.MAX_VALUE, 0, 0);
		require(large.size() == Integer.MAX_VALUE && !large.observe(1));
		System.out.println("PASS bounded growth, platform clamp, repeated counters and arithmetic limits");
	}
}
