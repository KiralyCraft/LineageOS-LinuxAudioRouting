package dev.kiraly.linuxaudio;
import android.app.PendingIntent;
import android.content.Intent;
import android.service.quicksettings.TileService;
public final class AudioTile extends TileService
{
	@Override
	public void onClick()
	{
		Intent intent = new Intent(this, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK).putExtra("start", true);
		startActivityAndCollapse(PendingIntent.getActivity(this, 2, intent, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE));
	}
}
