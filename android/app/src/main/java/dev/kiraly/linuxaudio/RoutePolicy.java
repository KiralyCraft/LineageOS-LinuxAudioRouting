package dev.kiraly.linuxaudio;

import java.io.IOException;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Reservations last until the Android stream has actually released its resources.
 */
final class RoutePolicy
{
	private static final class Claim
	{
		final String endpoint;
		final String resource;
		final String name;
		Claim(String endpoint, String resource, String name)
		{
			this.endpoint = endpoint;
			this.resource = resource;
			this.name = name;
		}
	}

	private final Map<Long, Claim> claims = new LinkedHashMap<>();

	synchronized String conflict(String endpoint, String resource)
	{
		for (Claim claim : claims.values())
			if (!claim.endpoint.equals(endpoint) && claim.resource.equals(resource))
				return "Audio path in use by " + claim.name;
		return "";
	}

	synchronized void reserve(long id, String endpoint, String resource, String name) throws IOException
	{
		if (claims.containsKey(id))
			throw new IOException("Duplicate stream reservation");
		for (Claim claim : claims.values())
			if (claim.resource.equals(resource))
				throw new IOException("Audio path in use by " + claim.endpoint);
		claims.put(id, new Claim(endpoint, resource, name));
	}

	synchronized void release(long id)
	{
		claims.remove(id);
	}
}
