// An HTTP call out of a DayZ server, and the answer, which arrives somewhere
// else and later.
//
// Two classes, and that is the whole point of the template. SUDO_ApiClient
// sends. SUDO_ApiCallback receives, on its own three entry points, some time
// after the send returned. There is no wire from one to the other: GET() hands
// back the state at submission, not the reply, and the engine calls the
// callback object when the answer lands.
//
// Both live in 3_Game because RestApi, RestContext and RestCallback are
// declared in 3_game/http/restapi.c, and a subclass cannot precede its base.
//
// Server only, by intent rather than by the API. An outbound call from every
// client is one call per player instead of one, puts the endpoint and any key
// in every client's memory, and gives the client an answer it can rewrite. The
// guard that enforces it is in 5_Mission/SUDO_ApiMission.c.
//
// GET_now and POST_now exist and this template refuses them. Both are marked
// "thread blocking operation!" in restapi.c, which on a dedicated server means
// the simulation thread stops for the whole round trip, including the full
// connection timeout against a dead endpoint.
//
// Set BASE_URL to your own endpoint before you build.

class SUDO_ApiCallback extends RestCallback
{
	string m_Path;

	override void OnSuccess(string data, int dataSize)
	{
		Print("[SUDO_Api] " + m_Path + " ok, " + dataSize + " bytes");
		// Print stops at 1024 bytes and says nothing about having stopped, so
		// the body is logged bounded rather than whole.
		if (dataSize > 512)
			Print("[SUDO_Api] first 512 bytes: " + data.Substring(0, 512));
		else
			Print("[SUDO_Api] " + data);

		SUDO_ApiClient.Release(this);
	}

	override void OnError(int errorCode)
	{
		// restapi.c: "May be called multiple times in case of (RetryCount > 1)".
		// Release checks membership first, so a second call is not a second
		// removal.
		Print("[SUDO_Api] " + m_Path + " failed, ERestResultState " + errorCode);
		SUDO_ApiClient.Release(this);
	}

	override void OnTimeout()
	{
		// A timeout has its own entry point. It never reaches OnError.
		Print("[SUDO_Api] " + m_Path + " timed out");
		SUDO_ApiClient.Release(this);
	}
}

class SUDO_ApiClient extends Managed
{
	// Scheme, host and the folder every call sits under. The per call path goes
	// to Request below, which is how GetRestContext and the request string
	// split the URL between them.
	static const string BASE_URL = "http://127.0.0.1:8080/dayz/";

	static ref SUDO_ApiClient s_Instance;

	// Every request in flight, held so it survives long enough to be answered.
	// RestCallback derives from Managed, which is script refcounted, so a
	// callback held only by a local drops to zero references when the method
	// that made it returns. That is long before any reply arrives, and it is
	// why the example in restapi.c cannot be copied as it stands.
	ref array<ref SUDO_ApiCallback> m_Pending;

	void SUDO_ApiClient()
	{
		m_Pending = new array<ref SUDO_ApiCallback>();
	}

	static SUDO_ApiClient Get()
	{
		if (!s_Instance)
			s_Instance = new SUDO_ApiClient();

		return s_Instance;
	}

	void Request(string path)
	{
		// The engine owns the RestApi. It is created out of hive init, so this
		// asks for it and never calls CreateRestApi or DestroyRestApi.
		RestApi api = GetRestApi();
		if (!api)
		{
			Print("[SUDO_Api] no RestApi on this build");
			return;
		}

		// Contexts are pooled by base URL and owned by the RestApi. RestContext
		// has a private constructor and a private destructor, so script can
		// neither make one nor delete one.
		RestContext ctx = api.GetRestContext(BASE_URL);
		if (!ctx)
		{
			Print("[SUDO_Api] no context for " + BASE_URL);
			return;
		}

		// Content-Type only, and the default is application/octet-stream, so a
		// JSON endpoint needs this line.
		ctx.SetHeader("application/json");

		SUDO_ApiCallback cb = new SUDO_ApiCallback();
		cb.m_Path = path;
		m_Pending.Insert(cb);

		// The int this returns is the state at submission, EREST_PENDING. It is
		// not the answer.
		ctx.GET(cb, path);
	}

	static void Release(SUDO_ApiCallback cb)
	{
		if (!s_Instance || !cb)
			return;

		int at = s_Instance.m_Pending.Find(cb);
		if (at > -1)
			s_Instance.m_Pending.Remove(at);
	}
}
