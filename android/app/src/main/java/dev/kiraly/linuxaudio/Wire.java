package dev.kiraly.linuxaudio;

import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.ParcelFileDescriptor;
import android.system.Os;
import android.system.OsConstants;
import org.json.JSONObject;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.FileDescriptor;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;

final class Wire implements AutoCloseable
{
	static final String SOCKET = "linux-audio-broker-v1";
	static final int VERSION = 1;
	static final int PCM_MAGIC = 0x50445541;
	static final int AUDIO = 1;
	static final int CLOCK = 2;
	final LocalSocket socket;
	final InputStream input;
	final OutputStream output;
	private final Object writer = new Object();
	private final byte[] receiveHeader = new byte[40];
	private final ByteBuffer transmitHeader = ByteBuffer.allocate(40).order(ByteOrder.LITTLE_ENDIAN);
	private ParcelFileDescriptor inputDescriptor;

	Wire() throws IOException
	{
		socket = new LocalSocket();
		try
		{
			socket.connect(new LocalSocketAddress(SOCKET, LocalSocketAddress.Namespace.ABSTRACT));
			if (socket.getPeerCredentials().getUid() != 0)
				throw new IOException("Audio broker is not root-owned");
			socket.setSoTimeout(7000);
			input = socket.getInputStream();
			output = socket.getOutputStream();
		}
		catch (IOException failure)
		{
			socket.close();
			throw failure;
		}
	}

	void hello(String role) throws Exception
	{
		JSONObject hello = new JSONObject().put("op", "hello").put("version", VERSION).put("role", role);
		send(hello);
		if (!receive().optString("op").equals("ready"))
			throw new IOException("Audio broker rejected this helper");
	}

	Wire attach(long stream, String token) throws Exception
	{
		send(new JSONObject().put("op", "hello").put("version", VERSION).put("role", "data").put("side", "android").put("stream", stream).put("token", token));
		if (!receive().optString("op").equals("ready"))
			throw new IOException("Stream capability rejected");
		if (input.read() != 'F')
			throw new IOException("Missing direct PCM descriptor");
		FileDescriptor[] descriptors = socket.getAncillaryFileDescriptors();
		if (descriptors == null)
			throw new IOException("Expected one direct PCM descriptor");
		if (descriptors.length != 1)
		{
			for (FileDescriptor descriptor : descriptors)
				try
				{
					Os.close(descriptor);
				}
				catch (Exception ignored)
				{
				}
			throw new IOException("Expected one direct PCM descriptor");
		}
		Wire direct;
		try
		{
			direct = new Wire(descriptors[0]);
		}
		finally
		{
			Os.close(descriptors[0]);
		}
		close();
		return direct;
	}

	private Wire(FileDescriptor descriptor) throws IOException
	{
		socket = null;
		inputDescriptor = ParcelFileDescriptor.dup(descriptor);
		input = new ParcelFileDescriptor.AutoCloseInputStream(inputDescriptor);
		output = new ParcelFileDescriptor.AutoCloseOutputStream(ParcelFileDescriptor.dup(descriptor));
	}

	void send(JSONObject message) throws IOException
	{
		byte[] bytes = message.toString().getBytes(StandardCharsets.UTF_8);
		if (bytes.length == 0 || bytes.length > 65536)
			throw new IOException("Control message too large");
		byte[] header = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(bytes.length).array();
		synchronized (writer)
		{
			output.write(header);
			output.write(bytes);
			output.flush();
		}
	}

	JSONObject receive() throws Exception
	{
		byte[] header = read(4);
		int count = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN).getInt();
		if (count < 1 || count > 65536)
			throw new IOException("Invalid control frame");
		return new JSONObject(new String(read(count), StandardCharsets.UTF_8));
	}

	byte[] read(int count) throws IOException
	{
		byte[] bytes = new byte[count];
		readInto(bytes, count);
		return bytes;
	}

	void readInto(byte[] bytes, int count) throws IOException
	{
		int offset = 0;
		while (offset < count)
		{
			int got = input.read(bytes, offset, count - offset);
			if (got < 0)
				throw new EOFException("Audio connection closed");
			offset += got;
		}
	}

	static final class Packet
	{
		int kind;
		long epoch;
		long frame;
		long time;
		int frames;
		int length;
		final byte[] data = new byte[65536];
	}

	void receivePcm(Packet packet) throws IOException
	{
		readInto(receiveHeader, 40);
		ByteBuffer header = ByteBuffer.wrap(receiveHeader).order(ByteOrder.LITTLE_ENDIAN);
		if (header.getInt() != PCM_MAGIC)
			throw new IOException("PCM header mismatch");
		packet.kind = header.getInt();
		packet.epoch = header.getLong();
		packet.frame = header.getLong();
		packet.time = header.getLong();
		packet.frames = header.getInt();
		int count = header.getInt();
		if (count < 0 || count > 65536 || (packet.kind != AUDIO && packet.kind != CLOCK))
			throw new IOException("Invalid PCM frame");
		packet.length = count;
		readInto(packet.data, count);
	}

	void sendPcm(int kind, long epoch, long frame, long time, int frames, byte[] bytes, int count) throws IOException
	{
		if (count < 0 || count > 65536 || count > bytes.length)
			throw new IOException("PCM length overflow");
		synchronized (writer)
		{
			transmitHeader.clear();
			transmitHeader.putInt(PCM_MAGIC).putInt(kind).putLong(epoch).putLong(frame).putLong(time).putInt(frames).putInt(count);
			output.write(transmitHeader.array());
			output.write(bytes, 0, count);
			output.flush();
		}
	}

	@Override
	public void close()
	{
		try
		{
			if (socket != null)
				socket.close();
			else
			{
				try
				{
					Os.shutdown(inputDescriptor.getFileDescriptor(), OsConstants.SHUT_RDWR);
				}
				catch (Exception ignored)
				{
				}
				try
				{
					input.close();
				}
				finally
				{
					output.close();
				}
			}
		}
		catch (IOException ignored)
		{
		}
	}
}
