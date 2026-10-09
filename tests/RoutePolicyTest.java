package dev.kiraly.linuxaudio;

import java.io.IOException;

public final class RoutePolicyTest
{
	private static void reject(RoutePolicy policy, long id, String endpoint, String resource) throws Exception
	{
		try
		{
			policy.reserve(id, endpoint, resource, endpoint, false);
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
		policy.reserve(1, "top", "capture.raw", "Phone top", false);
		reject(policy, 2, "bottom", "capture.raw");
		reject(policy, 2, "top", "capture.raw");
		policy.reserve(3, "headset-mic", "capture.communication", "Headset mic", true);
		policy.reserve(4, "speaker", "playback.fast", "Speakers", false);
		policy.reserve(5, "stereo", "playback.buffered", "Stereo", false);
		reject(policy, 6, "earpiece", "playback.fast");
		policy.release(2); // failed request cannot release the existing owner's claim
		reject(policy, 7, "bottom", "capture.raw");
		if (!policy.conflict("top", "capture.raw").isEmpty() || policy.conflict("bottom", "capture.raw").isEmpty())
			throw new AssertionError("Owner and waiting route must have distinct availability");
		policy.release(1); // called only after native stream retirement
		policy.reserve(8, "bottom", "capture.raw", "Phone bottom", false);
		policy.release(1); // late duplicate completion must not release the replacement
		reject(policy, 9, "top", "capture.raw");
		reject(policy, 8, "unrelated", "unused");
		if (!policy.requiresCommunication())
			throw new AssertionError("Live headset microphone must hold routing");
		policy.release(4);
		policy.reserve(10, "headset-out", "playback.fast", "Headset output", true);
		policy.release(3);
		if (!policy.requiresCommunication())
			throw new AssertionError("Playback still owns communication after mic retires");
		policy.release(10);
		if (policy.requiresCommunication())
			throw new AssertionError("Last headset retirement must release routing despite other streams");
		policy.reserve(11, "headset-mic", "capture.communication", "Reopened mic", true);
		policy.release(3);
		if (!policy.requiresCommunication())
			throw new AssertionError("Late old retirement must not release new headset session");
		policy.release(11);
		if (policy.requiresCommunication())
			throw new AssertionError("Failed or cancelled final setup must release routing");
		System.out.println("PASS raw exclusivity, shared-path playback exclusion, independent duplex routes, stale retirement and stream-scoped communication lifetime");
	}
}
