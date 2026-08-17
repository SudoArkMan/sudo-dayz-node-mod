// Where the request is made from, and the guard that keeps it off the client.
//
// MissionServer is constructed only by CreateCustomMission in the mission's own
// init.c, which runs on the server, so the guard below is belt and braces. It
// stays because the class this file reopens is the one place somebody will
// paste a second call, and a call that reaches every client is the mistake this
// template exists to prevent.

modded class MissionServer
{
	override void OnInit()
	{
		super.OnInit();

		if (!GetGame().IsServer())
		{
			return;
		}

		SUDO_ApiClient client = SUDO_ApiClient.Get();
		client.Request("status");
	}

	// >>> user code, kept when the graph regenerates
	// helpers you write here are preserved
	// <<< user code
};
