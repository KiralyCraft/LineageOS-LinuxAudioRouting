package dev.kiraly.linuxaudio;

import java.io.IOException;

public final class RoutePolicyTest
{
	private static void reject(RoutePolicy policy, long id, String endpoint, String resource) throws Exception
	{
		try
		{
			policy.reserve(id, endpoint, resource, endpoint);
		}
		catch (IOException expected)
		{
			return;
		}
		throw new AssertionError("Conflicting reservation accepted");
	}

	public static void main(String[] args) throws Exception
	{
		RoutePolicy policy = new RoutePolicy();
		policy.reserve(1, "top", "capture.raw", "Phone top");
		reject(policy, 2, "bottom", "capture.raw");
		reject(policy, 2, "top", "capture.raw");
		policy.reserve(3, "headset-mic", "capture.communication", "Headset mic");
		policy.reserve(4, "speaker", "playback.fast", "Speakers");
		policy.reserve(5, "stereo", "playback.buffered", "Stereo");
		reject(policy, 6, "earpiece", "playback.fast");
		policy.release(2); // failed request cannot release the existing owner's claim
		reject(policy, 7, "bottom", "capture.raw");
		if (!policy.conflict("top", "capture.raw").isEmpty() || policy.conflict("bottom", "capture.raw").isEmpty())
			throw new AssertionError("Owner and waiting route must have distinct availability");
		policy.release(1); // called only after native stream retirement
		policy.reserve(8, "bottom", "capture.raw", "Phone bottom");
		policy.release(1); // late duplicate completion must not release the replacement
		reject(policy, 9, "top", "capture.raw");
		reject(policy, 8, "unrelated", "unused");
		System.out.println("PASS raw exclusivity, shared-path playback exclusion, independent duplex routes and stale retirement");
	}
}
