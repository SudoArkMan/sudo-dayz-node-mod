// An HTTP call out of a DayZ server, and the answer, which arrives somewhere
// else and later.
//
// Two classes, and that is the whole point of the template. SUDO_ApiClient
// sends. SUDO_ApiCallback receives, on its own three entry points, some time
// after the send returned. There is no wire from one to the other: GET() hands
// back the state at submission, not the reply, and the engine calls the
// callback object when the answer lands. In the graph that is two separate
// exec chains on two separate classes, and no arrangement of nodes will join
// them, because nothing joins them at runtime either.
//
// Both classes live in 3_Game because RestApi, RestContext and RestCallback are
// declared in 3_game/http/restapi.c, and a subclass cannot precede its base.
//
// Server only, by intent rather than by the API. restapi.c is in 3_game and
// carries no guard of its own, so this compiles on both sides. An outbound call
// from every client is one call per player instead of one, puts the endpoint
// and any key in every client's memory, and gives the client an answer it can
// rewrite. The guard that enforces it is in 5_Mission/SUDO_ApiMission.c.
//
// GET_now and POST_now exist and this template refuses them. Both are marked
// "thread blocking operation!" in restapi.c, which on a dedicated server means
// the simulation thread stops for the whole round trip, including the full
// connection timeout against a dead endpoint.
//
// The members, which the editor cannot carry a comment above:
//
//   BASE_URL    scheme, host, and the folder every call sits under. The per
//               call path goes to Request, which is how GetRestContext and the
//               request string split a URL between them. This is the one line
//               to edit before you build.
//   s_Instance  the client, made on first use.
//   m_Pending   every request in flight, held so it lives long enough to be
//               answered. RestCallback derives from Managed, which is script
//               refcounted, so a callback held only by a local drops to zero
//               references when the method that made it returns. That is long
//               before any reply arrives, and it is why the example in
//               restapi.c cannot be copied as it stands.

class SUDO_ApiCallback extends RestCallback
{
	string m_Path;

	override void OnSuccess(string data, int dataSize)
	{
		Print("[SUDO_Api] " + m_Path + " ok, " + dataSize + " bytes");
		// Print stops at 1024 bytes and says nothing about having stopped, so
		// the body is logged bounded rather than whole. Expansion measured the
		// real limit at 1026 on the server and 240 on the client.
		if (dataSize > 512)
		{
			Print("[SUDO_Api] first 512 bytes: " + data.Substring(0, 512));
		}
		else
		{
			Print("[SUDO_Api] " + data);
		}

		SUDO_ApiClient.Release(this);
	}

	override void OnError(int errorCode)
	{
		// restapi.c on this method: "May be called multiple times in case of
		// (RetryCount > 1)". Release checks membership first, so a second call
		// is not a second removal.
		//
		// errorCode is an ERestResultState. Anything at or above EREST_ERROR is
		// a failure, and the enum separates client, server, app, timeout and
		// not implemented.
		Print("[SUDO_Api] " + m_Path + " failed, ERestResultState " + errorCode);
		SUDO_ApiClient.Release(this);
	}

	override void OnTimeout()
	{
		// A timeout has an entry point of its own and never reaches OnError.
		// The two timeouts are set with RestApi.SetOption using
		// ERESTOPTION_READOPERATION and ERESTOPTION_CONNECTION, both ten
		// seconds by default, both clamped to between three and 120.
		Print("[SUDO_Api] " + m_Path + " timed out");
		SUDO_ApiClient.Release(this);
	}

	// >>> user code, kept when the graph regenerates
	// helpers you write here are preserved
	// <<< user code
};

class SUDO_ApiClient extends Managed
{
	static const string BASE_URL = "http://127.0.0.1:8080/dayz/";
	static ref SUDO_ApiClient s_Instance;
	ref array<ref SUDO_ApiCallback> m_Pending;

	void SUDO_ApiClient()
	{
		m_Pending = new array<ref SUDO_ApiCallback>();
	}

	static SUDO_ApiClient Get()
	{
		if (!s_Instance)
		{
			s_Instance = new SUDO_ApiClient();
		}

		return s_Instance;
	}

	void Request(string path)
	{
		// The engine owns the RestApi. It is created out of hive init, so this
		// asks for it and never calls CreateRestApi or DestroyRestApi: tearing
		// down something the engine made is not script's to do. Nothing in all
		// of P:\scripts calls any of the three, so there is no vanilla usage to
		// copy either.
		RestApi api = GetRestApi();
		if (!api)
		{
			Print("[SUDO_Api] no RestApi on this build");
			return;
		}

		// Contexts are pooled by base URL and owned by the RestApi, which is
		// what "Get new or existing context" means. RestContext has a private
		// constructor and a private destructor, so script can neither make one
		// nor delete one.
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
		// not the answer. The answer arrives on cb.
		ctx.GET(cb, path);
	}

	static void Release(SUDO_ApiCallback cb)
	{
		// Called from all three completion paths, and from OnError more than
		// once when a request is retried, so it checks before it removes.
		if (!s_Instance || !cb)
		{
			return;
		}

		int at = s_Instance.m_Pending.Find(cb);
		if (at > -1)
		{
			s_Instance.m_Pending.Remove(at);
		}
	}

	// >>> user code, kept when the graph regenerates
	// helpers you write here are preserved
	// <<< user code
};
