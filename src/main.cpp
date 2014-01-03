#include <thread>
#include <vector>
#include <mutex>

#include "main.h"
#include "bcrypt.h"

using namespace samp_sdk;

logprintf_t logprintf;
extern void *pAMXFunctions;

std::vector<AMX*> p_Amx;
std::mutex bcrypt_queue_mutex;

void bcrypt_error(std::string funcname, std::string error)
{
	logprintf("bcrypt error: %s (Called from %s)", error.c_str(), funcname.c_str());
}

void thread_generate_bcrypt(int thread_idx, int thread_id, std::string buffer, short cost)
{
	bcrypt *crypter = new bcrypt();

	crypter
		->setCost(cost)
		->setPrefix("2y")
		->setKey(buffer);

	std::string hash = crypter->generate();

	delete(crypter);

	std::lock_guard<std::mutex> lock(bcrypt_queue_mutex);

	// Add the result to the queue
	bcrypt_queue.push_back({ BCRYPT_QUEUE_HASH, thread_idx, thread_id, hash, false });
}

// native bcrypt_hash(thread_idx, thread_id, password[], cost);
cell AMX_NATIVE_CALL bcrypt_hash(AMX* amx, cell* params)
{
	// Require 4 parameters
	if (params[0] != 4 * sizeof(cell))
	{
		bcrypt_error("bcrypt_hash", "Incorrect number of parameters (4 required)");
		return 0;
	}

	// Get the parameters
	int thread_idx = (int) params[1];
	int thread_id = (int) params[2];
	unsigned short cost = (unsigned short) params[4];

	if (cost < 4 || cost > 31)
	{
		bcrypt_error("bcrypt_hash", "Invalid work factor (cost). Allowed range: 4-31");
		return 0;
	}

	std::string password = "";

	int len = NULL;
	cell *addr = NULL;

	amx_GetAddr(amx, params[3], &addr);
	amx_StrLen(addr, &len);

	if (len++)
	{
		char *buffer = new char[len];
		amx_GetString(buffer, addr, 0, len);

		password = std::string(buffer);

		delete [] buffer;
	}

	// Start a new thread
	std::thread t(thread_generate_bcrypt, thread_idx, thread_id, password, cost);

	//�Leave the thread running
	t.detach();
	return 1;
}

void thread_check_bcrypt(int thread_idx, int thread_id, std::string password, std::string hash)
{
	bool match;
	match = bcrypt::compare(password, hash);

	std::lock_guard<std::mutex> lock(bcrypt_queue_mutex);

	// Add the result to the queue
	bcrypt_queue.push_back({ BCRYPT_QUEUE_CHECK, thread_idx, thread_id, "", match });
}

// native bcrypt_check(thread_idx, thread_id, const password[], const hash[]);
cell AMX_NATIVE_CALL bcrypt_check(AMX* amx, cell* params)
{
	// Require 4 parameters
	if (params[0] != 4 * sizeof(cell))
	{
		bcrypt_error("bcrypt_check", "Incorrect number of parameters (4 required)");
		return 0;
	}

	// Get the parameters
	int thread_idx = (int) params[1];
	int thread_id = (int) params[2];

	std::string password = "";
	std::string hash = "";

	int len[2] = { NULL };
	cell *addr[2] = { NULL };

	amx_GetAddr(amx, params[3], &addr[0]);
	amx_StrLen(addr[0], &len[0]);

	amx_GetAddr(amx, params[4], &addr[1]);
	amx_StrLen(addr[1], &len[1]);

	if (len[0]++)
	{
		char *buffer = new char[len[0]];
		amx_GetString(buffer, addr[0], 0, len[0]);

		password = std::string(buffer);

		delete [] buffer;
	}

	if (len[1]++)
	{
		char *buffer = new char[len[1]];
		amx_GetString(buffer, addr[1], 0, len[1]);

		hash = std::string(buffer);

		delete [] buffer;
	}

	// Start a new thread
	std::thread t(thread_check_bcrypt, thread_idx, thread_id, password, hash);

	//�Leave the thread running
	t.detach();
	return 1;
}

PLUGIN_EXPORT unsigned int PLUGIN_CALL Supports()
{
	return SUPPORTS_VERSION | SUPPORTS_PROCESS_TICK | SUPPORTS_AMX_NATIVES;
}

PLUGIN_EXPORT bool PLUGIN_CALL Load(void **ppData)
{
	pAMXFunctions = ppData[PLUGIN_DATA_AMX_EXPORTS];
	logprintf = (logprintf_t) ppData[PLUGIN_DATA_LOGPRINTF];

	unsigned max_threads = std::thread::hardware_concurrency();

	logprintf("  plugin.bcrypt "BCRYPT_VERSION" was loaded.");
	logprintf("  plugin.bcrypt: Concurred threads supported: %d", max_threads);
	return true;
}

PLUGIN_EXPORT void PLUGIN_CALL Unload()
{
	p_Amx.clear();

	logprintf("plugin.bcrypt: Plugin unloaded.");
}

PLUGIN_EXPORT void PLUGIN_CALL ProcessTick()
{
	if (bcrypt_queue.size() > 0)
	{
		std::lock_guard<std::mutex> lock(bcrypt_queue_mutex);

		int amx_idx;
		for (std::vector<AMX*>::iterator a = p_Amx.begin(); a != p_Amx.end(); ++a)
		{
			for (std::vector<bcrypt_queue_item>::iterator t = bcrypt_queue.begin(); t != bcrypt_queue.end(); ++t)
			{
				if ((*t).type == BCRYPT_QUEUE_HASH)
				{
					// public OnBcryptHashed(thread_idx, thread_id, const hash[]);

					if (!amx_FindPublic(*a, "OnBcryptHashed", &amx_idx))
					{
						// Push the hash
						cell addr;
						amx_PushString(*a, &addr, NULL, (*t).hash.c_str(), NULL, NULL);

						// Push the thread_id and thread_idx
						amx_Push(*a, (*t).thread_id);
						amx_Push(*a, (*t).thread_idx);

						// Execute and release memory
						amx_Exec(*a, NULL, amx_idx);
						amx_Release(*a, addr);
					}
				}
				else if ((*t).type == BCRYPT_QUEUE_CHECK)
				{
					// public OnBcryptChecked(thread_idx, thread_id, bool:match);

					if (!amx_FindPublic(*a, "OnBcryptChecked", &amx_idx))
					{
						// Push the thread_id and thread_idx
						amx_Push(*a, (*t).match);
						amx_Push(*a, (*t).thread_id);
						amx_Push(*a, (*t).thread_idx);

						// Execute and release memory
						amx_Exec(*a, NULL, amx_idx);
					}
				}
			}
		}

		// Clear the queue
		bcrypt_queue.clear();
	}
}

AMX_NATIVE_INFO PluginNatives [] =
{
	{"bcrypt_hash", bcrypt_hash},
	{"bcrypt_check", bcrypt_check },
	{ 0, 0 }
};

PLUGIN_EXPORT int PLUGIN_CALL AmxLoad(AMX *amx)
{
	p_Amx.push_back(amx);
	return amx_Register(amx, PluginNatives, -1);
}


PLUGIN_EXPORT int PLUGIN_CALL AmxUnload(AMX *amx)
{
	for (std::vector<AMX*>::iterator i = p_Amx.begin(); i != p_Amx.end(); ++i)
	{
		if (*i == amx)
		{
			p_Amx.erase(i);
			break;
		}
	}

	return AMX_ERR_NONE;
}