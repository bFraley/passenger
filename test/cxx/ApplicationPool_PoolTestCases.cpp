#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <sys/types.h>
#include <signal.h>
#include <utime.h>

#include "Utils/Timer.h"

/**
 * This file is used as a template to test the different ApplicationPool::Interface implementations.
 * It is #included in ApplicationPool_PoolTest.cpp and ApplicationPool_Server_PoolTest.cpp
 */
#ifdef USE_TEMPLATE
	static void sendTestRequest(SessionPtr &session, const char *uri = "/foo/new") {
		string headers;
		#define ADD_HEADER(name, value) \
			headers.append(name); \
			headers.append(1, '\0'); \
			headers.append(value); \
			headers.append(1, '\0')
		ADD_HEADER("HTTP_HOST", "www.test.com");
		ADD_HEADER("QUERY_STRING", "");
		ADD_HEADER("REQUEST_URI", uri);
		ADD_HEADER("REQUEST_METHOD", "GET");
		ADD_HEADER("REMOTE_ADDR", "localhost");
		ADD_HEADER("SCRIPT_NAME", "");
		ADD_HEADER("PATH_INFO", uri);
		ADD_HEADER("PASSENGER_CONNECT_PASSWORD", session->getConnectPassword());
		session->sendHeaders(headers);
	}
	
	static SessionPtr spawnRackApp(ApplicationPool::Ptr pool, const char *appRoot) {
		PoolOptions options;
		options.appRoot = appRoot;
		options.appType = "rack";
		return pool->get(options);
	}
	
	static SessionPtr spawnWsgiApp(ApplicationPool::Ptr pool, const char *appRoot) {
		PoolOptions options;
		options.appRoot = appRoot;
		options.appType = "wsgi";
		return pool->get(options);
	}
	
	namespace {
		class ReloadLoggingSpawnManager: public SpawnManager {
		public:
			vector<string> reloadLog;
			
			ReloadLoggingSpawnManager(const string &spawnServerCommand,
				const ServerInstanceDir::GenerationPtr &generation,
				const AccountsDatabasePtr &accountsDatabase = AccountsDatabasePtr(),
				const string &rubyCommand = "ruby")
			: SpawnManager(spawnServerCommand, generation, accountsDatabase, rubyCommand)
			{ }
			
			virtual void reload(const string &appRoot) {
				reloadLog.push_back(appRoot);
				SpawnManager::reload(appRoot);
			}
		};
		
		struct SpawnRackAppFunction {
			ApplicationPool::Ptr pool;
			bool *done;
			SessionPtr *session;
			
			SpawnRackAppFunction() {
				done    = NULL;
				session = NULL;
			}

			void operator()() {
				PoolOptions options;
				options.appRoot = "stub/rack";
				options.appType = "rack";
				options.useGlobalQueue = true;
				SessionPtr session = pool->get(options);
				*done = true;
				if (this->session != NULL) {
					*this->session = session;
				}
			}
		};
	}
	
	TEST_METHOD(1) {
		// Calling ApplicationPool.get() once should return a valid Session.
		SessionPtr session(spawnRackApp(pool, "stub/rack"));
		sendTestRequest(session);
		session->shutdownWriter();

		int reader = session->getStream();
		string result(readAll(reader));
		session->closeStream();
		ensure(result.find("hello <b>world</b>") != string::npos);
	}
	
	TEST_METHOD(2) {
		// Verify that the pool spawns a new app, and that
		// after the session is closed, the app is kept around.
		SessionPtr session(spawnRackApp(pool, "stub/rack"));
		ensure_equals("Before the session was closed, the app was busy", pool->getActive(), 1u);
		ensure_equals("Before the session was closed, the app was in the pool", pool->getCount(), 1u);
		session.reset();
		ensure_equals("After the session is closed, the app is no longer busy", pool->getActive(), 0u);
		ensure_equals("After the session is closed, the app is kept around", pool->getCount(), 1u);
	}
	
	TEST_METHOD(3) {
		// If we call get() with an application root, then we close the session,
		// and then we call get() again with the same app group name,
		// then the pool should not have spawned more than 1 app in total.
		SessionPtr session(spawnRackApp(pool, "stub/rack"));
		session.reset();
		session = spawnRackApp(pool, "stub/rack");
		ensure_equals(pool->getCount(), 1u);
	}
	
	TEST_METHOD(4) {
		// If we call get() with an app group name, then we call get() again before closing
		// the session, then the pool will eventually have spawned 2 apps in total.
		SessionPtr session(spawnRackApp(pool, "stub/rack"));
		SessionPtr session2(spawnRackApp(pool2, "stub/rack"));
		EVENTUALLY(5,
			result = pool->getCount() == 2u;
		);
	}
	
	TEST_METHOD(5) {
		// If we call get() twice with different app group names,
		// then the pool should spawn two different apps.
		TempDirCopy c1("stub/rack", "rackapp1.tmp");
		TempDirCopy c2("stub/rack", "rackapp2.tmp");
		replaceStringInFile("rackapp2.tmp/config.ru", "world", "world 2");
		SessionPtr session = spawnRackApp(pool, "rackapp1.tmp");
		SessionPtr session2 = spawnRackApp(pool2, "rackapp2.tmp");
		ensure_equals("Before the sessions were closed, both apps were busy", pool->getActive(), 2u);
		ensure_equals("Before the sessions were closed, both apps were in the pool", pool->getCount(), 2u);
		
		sendTestRequest(session);
		string result = readAll(session->getStream());
		ensure("Session 1 belongs to the correct app", result.find("hello <b>world</b>") != string::npos);
		session.reset();
		
		sendTestRequest(session2);
		result = readAll(session2->getStream());
		ensure("Session 2 belongs to the correct app", result.find("hello <b>world 2</b>") != string::npos);
		session2.reset();
	}
	
	TEST_METHOD(6) {
		// If we call get() twice with different app group names,
		// and we close both sessions, then both 2 apps should still
		// be in the pool.
		TempDirCopy c1("stub/rack", "rackapp1.tmp");
		TempDirCopy c2("stub/rack", "rackapp2.tmp");
		SessionPtr session(spawnRackApp(pool, "rackapp1.tmp"));
		SessionPtr session2(spawnRackApp(pool, "rackapp2.tmp"));
		session.reset();
		session2.reset();
		ensure_equals("There are 0 active apps", pool->getActive(), 0u);
		ensure_equals("There are 2 apps in total", pool->getCount(), 2u);
	}
	
	TEST_METHOD(7) {
		// If we call get() even though the pool is already full
		// (active == max), and the app group name is already
		// in the pool, then the pool must wait until there's an
		// inactive application.
		pool->setMax(1);
		// TODO: How do we test this?
	}
	
	TEST_METHOD(8) {
		// If ApplicationPool spawns a new instance,
		// and we kill it, then the next get() with the
		// same application root should not throw an exception:
		// ApplicationPool should spawn a new instance
		// after detecting that the original one died.
		SessionPtr session = spawnRackApp(pool, "stub/rack");
		kill(session->getPid(), SIGKILL);
		session.reset();
		usleep(20000); // Give the process some time to exit.
		spawnRackApp(pool, "stub/rack"); // should not throw
	}
	
	struct PoolWaitTestThread {
		ApplicationPool::Ptr pool;
		SessionPtr &m_session;
		bool &m_done;
		
		PoolWaitTestThread(const ApplicationPool::Ptr &pool,
			SessionPtr &session,
			bool &done)
		: m_session(session), m_done(done) {
			this->pool = pool;
			done = false;
		}
		
		void operator()() {
			m_session = spawnWsgiApp(pool, "stub/wsgi");
			m_done = true;
		}
	};

	TEST_METHOD(9) {
		// If we call get() even though the pool is already full
		// (active == max), and the app group name is *not* already
		// in the pool, then the pool will wait until enough sessions
		// have been closed.
		
		// Make the pool full.
		pool->setMax(2);
		SessionPtr session1 = spawnRackApp(pool, "stub/rack");
		SessionPtr session2 = spawnRackApp(pool2, "stub/rack");
		EVENTUALLY(5,
			result = pool->getCount() == 2u;
		);
		session1.reset();
		session2.reset();
		EVENTUALLY(5,
			result = pool->getActive() == 0u;
		);
		session1 = spawnRackApp(pool, "stub/rack");
		session2 = spawnRackApp(pool2, "stub/rack");
		ensure_equals(pool->getActive(), 2u);
		
		// Now spawn an app with a different app root.
		SessionPtr session3;
		bool done;
		TempThread thr(PoolWaitTestThread(pool2, session3, done));
		usleep(500000);
		ensure("ApplicationPool is still waiting", !done);
		ensure_equals(pool->getActive(), 2u);
		ensure_equals(pool->getCount(), 2u);
		
		// Now release one slot from the pool.
		session1.reset();
		
		// Session 3 should eventually be opened.
		EVENTUALLY(10,
			result = done;
		);
		ensure_equals(pool->getActive(), 2u);
		ensure_equals(pool->getCount(), 2u);
	}
	
	TEST_METHOD(10) {
		// If we call get(), and:
		// * the pool is already full, but there are inactive apps
		//   (active < count && count == max)
		// and
		// * the app group name for this get() is *not* already in the pool
		// then the an inactive app should be killed in order to
		// satisfy this get() command.
		TempDirCopy c1("stub/rack", "rackapp1.tmp");
		TempDirCopy c2("stub/rack", "rackapp2.tmp");
		
		// Make the pool full.
		pool->setMax(2);
		SessionPtr session1 = spawnRackApp(pool, "rackapp1.tmp");
		SessionPtr session2 = spawnRackApp(pool2, "rackapp1.tmp");
		EVENTUALLY(5,
			result = pool->getCount() == 2u;
		);
		session1.reset();
		session2.reset();
		EVENTUALLY(5,
			result = pool->getActive() == 0u;
		);
		
		// Now spawn a different app.
		session1 = spawnRackApp(pool, "rackapp2.tmp");
		ensure_equals(pool->getActive(), 1u);
		ensure_equals(pool->getCount(), 2u);
	}
	
	TEST_METHOD(11) {
		// A Session should still be usable after the pool has been destroyed.
		SessionPtr session = spawnRackApp(pool, "stub/rack");
		pool->clear();
		pool.reset();
		pool2.reset();
		
		sendTestRequest(session);
		session->shutdownWriter();
		
		int reader = session->getStream();
		string result = readAll(reader);
		session->closeStream();
		ensure(result.find("hello <b>world</b>") != string::npos);
	}
	
	TEST_METHOD(12) {
		// If tmp/restart.txt didn't exist but has now been created,
		// then the applications under app_root should be restarted.
		struct stat buf;
		TempDirCopy c("stub/rack", "rackapp.tmp");
		SessionPtr session1 = spawnRackApp(pool, "rackapp.tmp");
		SessionPtr session2 = spawnRackApp(pool2, "rackapp.tmp");
		session1.reset();
		session2.reset();
		EVENTUALLY(5,
			result = pool->getCount() == 2u;
		);
		
		touchFile("rackapp.tmp/tmp/restart.txt");
		spawnRackApp(pool, "rackapp.tmp");
		
		ensure_equals("No apps are active", pool->getActive(), 0u);
		ensure_equals("Both apps are killed, and a new one was spawned",
			pool->getCount(), 1u);
		ensure("Restart file still exists",
			stat("rackapp.tmp/tmp/restart.txt", &buf) == 0);
	}
	
	TEST_METHOD(13) {
		// If tmp/restart.txt was present, and its timestamp changed
		// since the last check, then the applications under the app group name
		// should still be restarted. However, a subsequent get()
		// should not result in a restart.
		pid_t old_pid;
		TempDirCopy c("stub/rack", "rackapp.tmp");
		TempDir d("rackapp.tmp/tmp/restart.txt");
		SessionPtr session = spawnRackApp(pool, "rackapp.tmp");
		old_pid = session->getPid();
		session.reset();
		EVENTUALLY(5,
			result = pool->getActive() == 0u;
		);
		
		touchFile("rackapp.tmp/tmp/restart.txt", 10);
		
		session = spawnRackApp(pool, "rackapp.tmp");
		ensure("The app was restarted", session->getPid() != old_pid);
		old_pid = session->getPid();
		session.reset();
		EVENTUALLY(5,
			result = pool->getActive() == 0u;
		);
		
		session = spawnRackApp(pool, "rackapp.tmp");
		ensure_equals("The app was not restarted",
			old_pid, session->getPid());
	}
	
	TEST_METHOD(15) {
		// Test whether restarting with restart.txt really results in code reload.
		TempDirCopy c("stub/rack", "rackapp.tmp");
		SessionPtr session = spawnRackApp(pool, "rackapp.tmp");
		sendTestRequest(session);
		string result = readAll(session->getStream());
		ensure(result.find("hello <b>world</b>") != string::npos);
		session.reset();
		EVENTUALLY(5,
			result = pool->getActive() == 0u;
		);
		
		touchFile("rackapp.tmp/tmp/restart.txt");
		replaceStringInFile("rackapp.tmp/config.ru", "world", "world 2");
		
		session = spawnRackApp(pool, "rackapp.tmp");
		sendTestRequest(session);
		result = readAll(session->getStream());
		ensure("App code has been reloaded", result.find("hello <b>world 2</b>") != string::npos);
	}
	
	TEST_METHOD(16) {
		// If tmp/always_restart.txt is present and is a file,
		// then the application under app_root should be always restarted.
		struct stat buf;
		pid_t old_pid;
		TempDirCopy c("stub/rack", "rackapp.tmp");
		SessionPtr session1 = spawnRackApp(pool, "rackapp.tmp");
		SessionPtr session2 = spawnRackApp(pool2, "rackapp.tmp");
		session1.reset();
		session2.reset();
		EVENTUALLY(5,
			result = pool->getActive() == 0u && pool->getCount() == 2u;
		);
		
		touchFile("rackapp.tmp/tmp/always_restart.txt");
		
		// This get() results in a restart.
		session1 = spawnRackApp(pool, "rackapp.tmp");
		old_pid = session1->getPid();
		session1.reset();
		EVENTUALLY(5,
			// First restart: no apps are active
			result = pool->getActive() == 0u;
		);
		ensure_equals("First restart: the first 2 apps were killed, and a new one was spawned",
			pool->getCount(), 1u);
		ensure("always_restart file has not been deleted",
			stat("rackapp.tmp/tmp/always_restart.txt", &buf) == 0);
		
		// This get() results in a restart as well.
		session1 = spawnRackApp(pool, "rackapp.tmp");
		ensure(old_pid != session1->getPid());
		session1.reset();
		EVENTUALLY(5,
			// Second restart: no apps are active
			result = pool->getActive() == 0u;
		);
		ensure_equals("Second restart: the last app was killed, and a new one was spawned",
			pool->getCount(), 1u);
		ensure("always_restart file has not been deleted",
			stat("rackapp.tmp/tmp/always_restart.txt", &buf) == 0);
	}
	
	TEST_METHOD(17) {
		// If tmp/always_restart.txt is present and is a directory,
		// then the application under app_root should be always restarted.
		struct stat buf;
		pid_t old_pid;
		TempDirCopy c("stub/rack", "rackapp.tmp");
		SessionPtr session1 = spawnRackApp(pool, "rackapp.tmp");
		SessionPtr session2 = spawnRackApp(pool2, "rackapp.tmp");
		session1.reset();
		session2.reset();
		EVENTUALLY(5,
			result = pool->getActive() == 0u && pool->getCount() == 2u;
		);
		
		TempDir d("rackapp.tmp/tmp/always_restart.txt");
		
		// This get() results in a restart.
		session1 = spawnRackApp(pool, "rackapp.tmp");
		old_pid = session1->getPid();
		session1.reset();
		EVENTUALLY(5,
			// First restart: no apps are active
			result = pool->getActive() == 0u;
		);
		ensure_equals("First restart: the first 2 apps were killed, and a new one was spawned",
			pool->getCount(), 1u);
		ensure("always_restart directory has not been deleted",
			stat("rackapp.tmp/tmp/always_restart.txt", &buf) == 0);
		
		// This get() results in a restart as well.
		session1 = spawnRackApp(pool, "rackapp.tmp");
		ensure(old_pid != session1->getPid());
		session1.reset();
		EVENTUALLY(5,
			// Second restart: no apps are active
			result = pool->getActive() == 0u;
		);
		ensure_equals("Second restart: the last app was killed, and a new one was spawned",
			pool->getCount(), 1u);
		ensure("always_restart directory has not been deleted",
			stat("rackapp.tmp/tmp/always_restart.txt", &buf) == 0);
	}
	
	TEST_METHOD(18) {
		// Test whether restarting with tmp/always_restart.txt really results in code reload.
		TempDirCopy c("stub/rack", "rackapp.tmp");
		SessionPtr session = spawnRackApp(pool, "rackapp.tmp");
		sendTestRequest(session);
		string result = readAll(session->getStream());
		ensure(result.find("hello <b>world</b>") != string::npos);
		session.reset();

		touchFile("rackapp.tmp/tmp/always_restart.txt");
		replaceStringInFile("rackapp.tmp/config.ru", "world", "world 2");
		
		session = spawnRackApp(pool, "rackapp.tmp");
		sendTestRequest(session);
		result = readAll(session->getStream());
		ensure("App code has been reloaded (1)", result.find("hello <b>world 2</b>") != string::npos);
		session.reset();
		EVENTUALLY(5,
			result = pool->getActive() == 0u;
		);
		
		replaceStringInFile("rackapp.tmp/config.ru", "world 2", "world 3");
		session = spawnRackApp(pool, "rackapp.tmp");
		sendTestRequest(session);
		result = readAll(session->getStream());
		ensure("App code has been reloaded (2)", result.find("hello <b>world 3</b>") != string::npos);
		session.reset();
	}
	
	TEST_METHOD(19) {
		// If tmp/restart.txt and tmp/always_restart.txt are present, 
		// the application under app_root should still be restarted and
		// both files must be kept.
		pid_t old_pid, pid;
		struct stat buf;
		TempDirCopy c("stub/rack", "rackapp.tmp");
		SessionPtr session1 = spawnRackApp(pool, "rackapp.tmp");
		SessionPtr session2 = spawnRackApp(pool2, "rackapp.tmp");
		session1.reset();
		session2.reset();
		EVENTUALLY(5,
			result = pool->getActive() == 0u && pool->getCount() == 2u;
		);
		
		touchFile("rackapp.tmp/tmp/restart.txt");
		touchFile("rackapp.tmp/tmp/always_restart.txt");
		
		old_pid = spawnRackApp(pool, "rackapp.tmp")->getPid();
		ensure("always_restart.txt file has not been deleted",
			stat("rackapp.tmp/tmp/always_restart.txt", &buf) == 0);
		ensure("restart.txt file has not been deleted",
			stat("rackapp.tmp/tmp/restart.txt", &buf) == 0);
		EVENTUALLY(5,
			result = pool->getActive() == 0u;
		);
		
		pid = spawnRackApp(pool, "rackapp.tmp")->getPid();
		ensure("The app was restarted", pid != old_pid);
	}
	
	TEST_METHOD(20) {
		// It should look for restart.txt in the directory given by
		// the restartDir option, if available.
		struct stat buf;
		char path[1024];
		PoolOptions options("stub/rack");
		options.appType = "rack";
		options.restartDir = string(getcwd(path, sizeof(path))) + "/stub/rack";
		
		SessionPtr session1 = pool->get(options);
		SessionPtr session2 = pool2->get(options);
		session1.reset();
		session2.reset();
		EVENTUALLY(5,
			result = pool->getActive() == 0u && pool->getCount() == 2u;
		);
		
		DeleteFileEventually f("stub/rack/restart.txt");
		touchFile("stub/rack/restart.txt");
		
		pool->get(options);
		
		ensure_equals("No apps are active", pool->getActive(), 0u);
		ensure_equals("Both apps are killed, and a new one was spawned",
			pool->getCount(), 1u);
		ensure("Restart file still exists",
			stat("stub/rack/restart.txt", &buf) == 0);
	}
	
	TEST_METHOD(21) {
		// restartDir may also be a directory relative to the
		// application root.
		struct stat buf;
		PoolOptions options("stub/rack");
		options.appType = "rack";
		options.restartDir = "public";
		
		SessionPtr session1 = pool->get(options);
		SessionPtr session2 = pool2->get(options);
		session1.reset();
		session2.reset();
		EVENTUALLY(5,
			result = pool->getActive() == 0u && pool->getCount() == 2u;
		);
		
		DeleteFileEventually f("stub/rack/public/restart.txt");
		touchFile("stub/rack/public/restart.txt");
		
		pool->get(options);
		
		ensure_equals("No apps are active", pool->getActive(), 0u);
		ensure_equals("Both apps are killed, and a new one was spawned",
			pool->getCount(), 1u);
		ensure("Restart file still exists",
			stat("stub/rack/public/restart.txt", &buf) == 0);
	}
	
	TEST_METHOD(22) {
		// The cleaner thread should clean idle applications.
		pool->setMaxIdleTime(1);
		spawnRackApp(pool, "stub/rack");
		EVENTUALLY(10,
			result = pool->getCount() == 0u;
		);
		
		time_t begin = time(NULL);
		while (pool->getCount() == 1u && time(NULL) - begin < 10) {
			usleep(100000);
		}
		ensure_equals("App should have been cleaned up", pool->getCount(), 0u);
	}
	
	TEST_METHOD(23) {
		// MaxPerApp is respected.
		pool->setMax(3);
		pool->setMaxPerApp(1);
		
		// We connect to stub/rack while it already has an instance with
		// 1 request in its queue. Assert that the pool doesn't spawn
		// another instance.
		SessionPtr session1 = spawnRackApp(pool, "stub/rack");
		SessionPtr session2 = spawnRackApp(pool2, "stub/rack");
		
		// We connect to stub/wsgi. Assert that the pool spawns a new
		// instance for this app.
		TempDirCopy c("stub/wsgi", "wsgiapp.tmp");
		ApplicationPool::Ptr pool3 = newPoolConnection();
		SessionPtr session3 = spawnWsgiApp(pool3, "wsgiapp.tmp");
		ensure_equals(pool->getCount(), 2u);
	}
	
	TEST_METHOD(24) {
		// Application instance is shutdown after 'maxRequests' requests.
		PoolOptions options("stub/rack");
		int reader;
		pid_t originalPid;
		SessionPtr session;
		
		options.appType = "rack";
		options.maxRequests = 4;
		pool->setMax(1);
		session = pool->get(options);
		originalPid = session->getPid();
		session.reset();
		EVENTUALLY(5,
			result = pool->getActive() == 0u;
		);
		
		for (unsigned int i = 0; i < 4; i++) {
			session = pool->get(options);
			sendTestRequest(session);
			session->shutdownWriter();
			reader = session->getStream();
			readAll(reader);
			// Must explicitly call reset() here because we
			// want to close the session right now.
			session.reset();
			EVENTUALLY(5,
				result = pool->getActive() == 0u;
			);
		}
		
		session = pool->get(options);
		ensure(session->getPid() != originalPid);
	}
	
	TEST_METHOD(25) {
		// If global queueing mode is enabled, then get() waits until
		// there's at least one idle backend process for this application
		// domain.
		pool->setMax(1);
		
		PoolOptions options;
		options.appRoot = "stub/rack";
		options.appType = "rack";
		options.useGlobalQueue = true;
		SessionPtr session = pool->get(options);
		
		bool done = false;
		SpawnRackAppFunction func;
		func.pool = pool2;
		func.done = &done;
		TempThread thr(func);
		
		// Previous session hasn't been closed yet, so pool should still
		// be waiting.
		usleep(100000);
		ensure("(1)", !done);
		ensure_equals("(2)", pool->getGlobalQueueSize(), 1u);
		ensure_equals("(3)", pool->getActive(), 1u);
		ensure_equals("(4)", pool->getCount(), 1u);
		
		// Close the previous session. The thread should now finish.
		session.reset();
		EVENTUALLY(5,
			result = done;
		);
	}
	
	TEST_METHOD(26) {
		// When a previous application group spinned down, and we touched
		// restart.txt and try to spin up a new process for this domain,
		// then any ApplicationSpawner/FrameworkSpawner processes should be
		// killed first.
		SessionPtr session;
		TempDirCopy c1("stub/rack", "rackapp1.tmp");
		TempDirCopy c2("stub/rack", "rackapp2.tmp");
		shared_ptr<ReloadLoggingSpawnManager> spawnManager(
			new ReloadLoggingSpawnManager("../helper-scripts/passenger-spawn-server", generation)
		);
		reinitializeWithSpawnManager(spawnManager);
		
		pool->setMax(1);
		session = spawnRackApp(pool, "rackapp1.tmp");
		session.reset();
		session = spawnRackApp(pool, "rackapp2.tmp");
		ensure_equals("rackapp2.tmp is not reloaded because restart.txt is not touched",
			spawnManager->reloadLog.size(), 0u);
		session.reset();
		EVENTUALLY(5,
			result = pool->getActive() == 0u;
		);
		
		touchFile("rackapp1.tmp/tmp/restart.txt");
		session = spawnRackApp(pool, "rackapp1.tmp");
		ensure_equals("rackapp1.tmp is reloaded because restart.txt is touched (1)",
			spawnManager->reloadLog.size(), 1u);
		ensure_equals("rackapp1.tmp is reloaded because restart.txt is touched (2)",
			spawnManager->reloadLog[0], "rackapp1.tmp");
	}
	
	TEST_METHOD(27) {
		// Test inspect()
		SessionPtr session1 = spawnRackApp(pool, "stub/rack");
		string str = pool->inspect();
		ensure("Contains 'max = '", str.find("max ") != string::npos);
		ensure("Contains PID", str.find("PID: " + toString(session1->getPid())) != string::npos);
	}
	
	TEST_METHOD(28) {
		// Test toXml(true)
		SessionPtr session1 = spawnRackApp(pool, "stub/rack");
		string xml = pool->toXml();
		ensure("Contains <process>", xml.find("<process>") != string::npos);
		ensure("Contains PID", xml.find("<pid>" + toString(session1->getPid()) + "</pid>") != string::npos);
		ensure("Contains sensitive information", xml.find("<server_sockets>") != string::npos);
	}
	
	TEST_METHOD(29) {
		// Test toXml(false)
		SessionPtr session1 = spawnRackApp(pool, "stub/rack");
		string xml = pool->toXml(false);
		ensure("Contains <process>", xml.find("<process>") != string::npos);
		ensure("Contains PID", xml.find("<pid>" + toString(session1->getPid()) + "</pid>") != string::npos);
		ensure("Does not contain sensitive information", xml.find("<server_sockets>") == string::npos);
	}
	
	TEST_METHOD(30) {
		// Test detach().
		
		// Create 2 processes, where only the first one is active.
		SessionPtr session1 = spawnRackApp(pool, "stub/rack");
		SessionPtr session2 = spawnRackApp(pool2, "stub/rack");
		session2.reset();
		EVENTUALLY(5,
			result = pool->getActive() == 1u && pool->getCount() == 2u;
		);
		
		// Make sure session2 refers to a different process than session1.
		session2 = spawnRackApp(pool2, "stub/rack");
		string session2dk = session2->getDetachKey();
		session2.reset();
		EVENTUALLY(5,
			result = pool->getActive() == 1u;
		);
		
		// First detach works. It was active so the 'active' property
		// is decremented.
		ensure("(10)", pool->detach(session1->getDetachKey()));
		ensure_equals("(11)", pool->getActive(), 0u);
		ensure_equals("(12)", pool->getCount(), 1u);
		
		// Second detach with the same identifier doesn't do anything.
		ensure("(20)", !pool->detach(session1->getDetachKey()));
		ensure_equals("(21)", pool->getActive(), 0u);
		ensure_equals("(22)", pool->getCount(), 1u);
		
		// Detaching an inactive process works too.
		ensure("(30)", pool->detach(session2dk));
		ensure_equals("(31)", pool->getActive(), 0u);
		ensure_equals("(32)", pool->getCount(), 0u);
	}
	
	TEST_METHOD(31) {
		// If the app group does not yet exist, and options.minProcesses > 0,
		// then get() will spawn 1 process immediately, return its session,
		// and spawn more processes in the background until options.minProcesses
		// is satisfied.
		TempDirCopy c1("stub/rack", "rackapp.tmp");
		PoolOptions options;
		options.appRoot = "rackapp.tmp";
		options.appType = "rack";
		options.minProcesses = 3;
		options.spawnMethod = "conservative";
		
		writeFile("rackapp.tmp/config.ru",
			"sleep 0.1\n"
			"run lambda {}\n");
		
		SessionPtr session1 = pool->get(options);
		ensure_equals(pool->getActive(), 1u);
		ensure_equals(pool->getCount(), 1u);
		
		EVENTUALLY(5,
			result = pool->getCount() == 3u;
		);
	}
	
	TEST_METHOD(32) {
		// If the app group already exists, all processes are active,
		// count < max, options.minProcesses > 0 and global queuing turned off,
		// then get() will check out an existing process immediately
		// and spawn new ones in the background until options.minProcesses
		// is satisfied.
		TempDirCopy c1("stub/rack", "rackapp.tmp");
		PoolOptions options;
		options.appRoot = "rackapp.tmp";
		options.appType = "rack";
		options.spawnMethod = "conservative";
		options.minProcesses = 3;
		pool->setMax(3);
		
		// Spawn a single process.
		SessionPtr session1 = pool->get(options);
		ensure_equals(pool->getActive(), 1u);
		ensure_equals(pool->getCount(), 1u);
		
		writeFile("rackapp.tmp/config.ru",
			"sleep 0.1\n"
			"run lambda {}\n");
		
		// Now call get(); this one will use the previous process
		// and spawn a new one in the background.
		SessionPtr session2 = pool2->get(options);
		ensure_equals(pool->getActive(), 1u);
		ensure_equals(pool->getCount(), 1u);
		ensure_equals(session1->getPid(), session2->getPid());
		
		EVENTUALLY(5,
			result = pool->getCount() == 3u;
		);
	}
	
	/* If the app group already exists, all processes are active,
	 * count < max, options.minProcesses > 0 and global queuing turned on,
	 * then get() will wait until either
	 * (1) an existing process becomes inactive.
	 * or until
	 * (2) a new process has been spawned.
	 */
	
	TEST_METHOD(33) {
		// Here we test scenario (1).
		PoolOptions options;
		options.appRoot = "stub/rack";
		options.appType = "rack";
		options.minProcesses = 3;
		options.useGlobalQueue = true;
		pool->setMax(3);
		
		ApplicationPool::Ptr pool3 = newPoolConnection();
		ApplicationPool::Ptr pool4 = newPoolConnection();
		
		// Spawn 3 processes.
		SessionPtr session1 = pool->get(options);
		SessionPtr session2 = pool2->get(options);
		session2.reset();
		EVENTUALLY(5,
			result = pool->getCount() == 3u;
		);
		
		// Make sure all of them are active.
		session2 = pool2->get(options);
		SessionPtr session3 = pool3->get(options);
		ensure_equals(pool->getActive(), 3u);
		ensure_equals(pool->getCount(), 3u);
		
		// Now call get() in a thread.
		SpawnRackAppFunction func;
		bool done = false;
		func.pool = pool4;
		func.done = &done;
		TempThread thr(func);
		
		usleep(20000);
		ensure("Still waiting on global queue", !done);
		ensure_equals(pool->getGlobalQueueSize(), 1u);
		
		// Make 1 process available.
		session1.reset();
		EVENTUALLY(5,
			result = done;
		);
	}
	
	TEST_METHOD(34) {
		// Here we test scenario (2).
		PoolOptions options;
		options.appRoot = "stub/rack";
		options.appType = "rack";
		options.minProcesses = 3;
		options.useGlobalQueue = true;
		pool->setMax(3);
		
		ApplicationPool::Ptr pool3 = newPoolConnection();
		ApplicationPool::Ptr pool4 = newPoolConnection();
		ApplicationPool::Ptr pool5 = newPoolConnection();
		
		// Spawn 3 processes.
		SessionPtr session1 = pool->get(options);
		SessionPtr session2 = pool2->get(options);
		session2.reset();
		EVENTUALLY(5,
			result = pool->getCount() == 3u;
		);
		
		// Make sure all of them are active.
		session2 = pool2->get(options);
		SessionPtr session3 = pool3->get(options);
		ensure_equals(pool->getActive(), 3u);
		ensure_equals(pool->getCount(), 3u);
		
		// Now call get() in a thread.
		SpawnRackAppFunction func1;
		SessionPtr session4;
		bool done1 = false;
		func1.pool = pool4;
		func1.done = &done1;
		func1.session = &session4;
		TempThread thr1(func1);
		
		// And again.
		SpawnRackAppFunction func2;
		SessionPtr session5;
		bool done2 = false;
		func2.pool = pool5;
		func2.done = &done2;
		func2.session = &session5;
		TempThread thr2(func2);
		
		// We should now arrive at a state where there are 3 processes, all
		// busy, and 2 threads waiting on the global queue.
		usleep(20000);
		ensure("Still waiting on global queue", !done1 && !done2);
		ensure_equals(pool->getGlobalQueueSize(), 2u);
		
		// Increasing the max will cause one of the threads to wake
		// up, start a spawn action in the background, and go to sleep
		// again. Eventually the new process will be done spawning,
		// causing one of the threads to wake up. The other one will
		// continue to wait.
		pool->setMax(4);
		EVENTUALLY(5,
			result = (done1 && !done2) || (!done1 && done2);
		);
	}
	
	TEST_METHOD(35) {
		// When spawning an app in the background, if it encountered an error
		// it will remove the whole app group.
		TempDirCopy c1("stub/rack", "rackapp.tmp");
		PoolOptions options;
		options.appRoot = "rackapp.tmp";
		options.appType = "rack";
		options.spawnMethod = "conservative";
		options.printExceptions = false;
		
		SessionPtr session1 = pool->get(options);
		
		writeFile("rackapp.tmp/config.ru",
			"raise 'foo'\n");
		pool2->get(options);
		
		EVENTUALLY(5,
			result = pool->getCount() == 0u;
		);
	}
	
	TEST_METHOD(36) {
		// When cleaning, at least options.minProcesses processes should be kept around.
		pool->setMaxIdleTime(0);
		ApplicationPool::Ptr pool3 = newPoolConnection();
		PoolOptions options;
		options.appRoot = "stub/rack";
		options.appType = "rack";
		options.minProcesses = 2;
		
		// Spawn 2 processes.
		SessionPtr session1 = pool->get(options);
		SessionPtr session2 = pool2->get(options);
		session1.reset();
		session2.reset();
		EVENTUALLY(5,
			result = pool->getActive() == 0u && pool->getCount() == 2u;
		);
		
		// Spawn another process, so we get 3.
		session1 = pool->get(options);
		session2 = pool2->get(options);
		SessionPtr session3 = pool3->get(options);
		session3.reset();
		EVENTUALLY(5,
			result = pool->getActive() == 2u && pool->getCount() == 3u;
		);
		
		// Now wait until one process is idle cleaned.
		pool->setMaxIdleTime(1);
		EVENTUALLY(10,
			result = pool->getCount() == 2u;
		);
	}
	
	TEST_METHOD(37) {
		// Test whether processes are grouped together by appGroupName.
		TempDirCopy c1("stub/rack", "rackapp.tmp");
		PoolOptions options1;
		options1.appRoot = "rackapp.tmp";
		options1.appType = "rack";
		options1.appGroupName = "group A";
		SessionPtr session1 = pool->get(options1);
		
		TempDirCopy c2("stub/rack", "rackapp2.tmp");
		PoolOptions options2;
		options2.appRoot = "rackapp2.tmp";
		options2.appType = "rack";
		options2.appGroupName = "group A";
		SessionPtr session2 = pool2->get(options2);
		
		session1.reset();
		session2.reset();
		EVENTUALLY(5,
			result = pool->getCount() == 2u;
		);
		
		touchFile("rackapp.tmp/tmp/restart.txt");
		session1 = pool->get(options1);
		ensure_equals(pool->getCount(), 1u);
	}
	
	/*************************************/
	
	TEST_METHOD(40) {
		// The maxInstances pool option is respected.
		pool->setMax(3);
		
		PoolOptions options;
		options.appRoot = "stub/rack";
		options.appType = "rack";
		options.maxInstances = 1;
		
		// We connect to stub/rack while it already has an instance with
		// 1 request in its queue. Assert that the pool doesn't spawn
		// another instance.
		SessionPtr session1 = pool->get(options);
		SessionPtr session2 = pool2->get(options);
		ensure_equals(pool->getCount(), 1u);
		
		// We connect to stub/wsgi. Assert that the pool spawns a new
		// instance for this app.
		ApplicationPool::Ptr pool3(newPoolConnection());
		SessionPtr session3 = spawnWsgiApp(pool3, "stub/wsgi");
		ensure_equals(pool->getCount(), 2u);
	}
	
	TEST_METHOD(41) {
		// Test rolling restarts.
		TempDirCopy c1("stub/rack", "rackapp1.tmp");
		PoolOptions options;
		options.appRoot = "rackapp1.tmp";
		options.appType = "rack";
		options.rollingRestart = true;
		
		// Spawn an app.
		SessionPtr session = pool->get(options);
		pid_t originalPid = session->getPid();
		session.reset();
		// Make sure that the pool knows that we've disconnected so
		// that the next get() doesn't try to spawn a new process.
		while (pool->getActive() > 0) {
			usleep(10000);
		}
		
		touchFile("rackapp1.tmp/tmp/restart.txt");
		writeFile("rackapp1.tmp/config.ru",
			"app = lambda do |env|\n"
			"  [200, { 'Content-Type' => 'text/html' }, ['hello world']]\n"
			"end\n"
			"\n"
			"while !File.exist?('continue.txt')\n"
			"  sleep 0.01\n"
			"end\n"
			"run app\n");
		
		// The new app won't finish spawning until we create continue.txt.
		// In the mean time, all get() commands should immediately return
		// the old process without blocking.
		Timer timer;
		while (timer.elapsed() < 500) {
			session = pool->get(options);
			ensure_equals(session->getPid(), originalPid);
			session.reset();
			
			// Don't overwhelm the application process's connect backlog.
			usleep(1000);
			// Make sure that the pool knows that we've disconnected so
			// that the next get() doesn't try to spawn a new process.
			while (pool->getActive() > 0) {
				usleep(5000);
			}
		}
		
		touchFile("rackapp1.tmp/continue.txt");
		timer.start();
		bool pidChanged = false;
		while (timer.elapsed() < 500 && !pidChanged) {
			session = pool->get(options);
			pidChanged = session->getPid() != originalPid;
			session.reset();
			usleep(1);
		}
		ensure(pidChanged);
	}
	
	TEST_METHOD(42) {
		// Test ignoreSpawnErrors and get().
		TempDirCopy c1("stub/rack", "rackapp1.tmp");
		PoolOptions options;
		options.appRoot = "rackapp1.tmp";
		options.appType = "rack";
		options.spawnMethod = "conservative";
		
		ApplicationPool::Ptr pool3 = newPoolConnection();
		
		// Spawn a process.
		SessionPtr session1 = pool->get(options);
		
		// Now fubar the app.
		writeFile("rackapp1.tmp/config.ru", "raise 'an error'");
		
		// The next get() will return a connection to the existing
		// process while another process is being spawned in the
		// background.
		options.ignoreSpawnErrors = true;
		options.printExceptions = false;
		SessionPtr session2 = pool2->get(options);
		ensure_equals("(1)", session2->getPid(), session1->getPid());
		session2.reset();
		
		// The pool will eventually notice that spawning has failed...
		usleep(500000);
		ensure_equals("(2)", pool->getActive(), 1u);
		ensure_equals("(3)", pool->getCount(), 1u);
		
		// ...and its group should then be flagged as 'bad' so that
		// another get() won't cause it to spawn a new process even
		// when all processes are active. Instead the pool should
		// continue to reuse existing processes.
		writeFile("rackapp1.tmp/config.ru", "run lambda { |env| [200, {}, ['']] }");
		session2 = pool2->get(options);
		ensure_equals("(4)", session2->getPid(), session1->getPid());
		
		usleep(500000);
		ensure_equals("(5)", pool->getActive(), 1u);
		ensure_equals("(6)", pool->getCount(), 1u);
		
		// Until the user explicitly restarts the app.
		touchFile("rackapp1.tmp/tmp/restart.txt");
		SessionPtr session3 = pool3->get(options);
		ensure("(7)", session3->getPid() != session1->getPid());
	}
	
	TEST_METHOD(43) {
		// Test ignoreSpawnErrors and rolling restarts.
		TempDirCopy c1("stub/rack", "rackapp1.tmp");
		PoolOptions options;
		options.appRoot = "rackapp1.tmp";
		options.appType = "rack";
		options.rollingRestart = true;
		options.minProcesses = 3;
		
		// Spawn 3 processes.
		ApplicationPool::Ptr pool3 = newPoolConnection();
		SessionPtr session1 = pool->get(options);
		EVENTUALLY(5,
			result = pool->getCount() == 3;
		);
		SessionPtr session2 = pool2->get(options);
		SessionPtr session3 = pool3->get(options);
		ensure_equals("(1)", pool->getActive(), 3u);
		
		pid_t orig_pid1 = session1->getPid();
		pid_t orig_pid2 = session2->getPid();
		pid_t orig_pid3 = session3->getPid();
		session1.reset();
		session2.reset();
		session3.reset();
		EVENTUALLY(5,
			result = pool->getActive() == 0u;
		);
		
		// Now fubar the app and flag restart.
		writeFile("rackapp1.tmp/config.ru", "raise 'an error'");
		touchFile("rackapp1.tmp/tmp/restart.txt");
		
		// Let the pool attempt restart in the background.
		options.ignoreSpawnErrors = true;
		options.printExceptions = false;
		pool->get(options);
		// Wait some time until it has detected the spawn error.
		sleep(1);
		
		// It should leave all the existing processes alone.
		ensure_equals(pool->getCount(), 3u);
		session1 = pool->get(options);
		session2 = pool2->get(options);
		session3 = pool3->get(options);
		pid_t pid1 = session1->getPid();
		pid_t pid2 = session2->getPid();
		pid_t pid3 = session3->getPid();
		ensure(pid1 != pid2);
		ensure(pid2 != pid3);
		ensure(pid1 == orig_pid1 || pid1 == orig_pid2 || pid1 == orig_pid3);
		ensure(pid2 == orig_pid1 || pid2 == orig_pid2 || pid2 == orig_pid3);
		ensure(pid3 == orig_pid1 || pid3 == orig_pid2 || pid3 == orig_pid3);
	}
	
	TEST_METHOD(44) {
		// Test sticky sessions.
		TempDirCopy c1("stub/rack", "rackapp1.tmp");
		writeFile("rackapp1.tmp/config.ru",
			"sticky_session_id = File.read('sticky_session_id.txt')\n"
			"app = lambda do |env|\n"
			"  [200,\n"
			"   { 'Content-Type' => 'text/plain', 'X-Stickiness' => sticky_session_id },\n"
			"   ['hello']"
			"  ]\n"
			"end\n"
			"run app\n");
		
		PoolOptions options;
		options.appRoot = "rackapp1.tmp";
		options.appType = "rack";
		options.spawnMethod = "conservative";
		
		// Setup 2 app process, one with sticky session ID 1234
		// and another with 5678.
		
		writeFile("rackapp1.tmp/sticky_session_id.txt", "1234");
		SessionPtr session1 = pool->get(options);
		pid_t app1_pid = session1->getPid();
		session1->setStickySessionId("1234");
		
		writeFile("rackapp1.tmp/sticky_session_id.txt", "5678");
		SessionPtr session2 = pool2->get(options);
		session2.reset();
		EVENTUALLY(5,
			result = pool2->getCount() == 2u;
		);
		session2 = pool2->get(options);
		pid_t app2_pid = session2->getPid();
		session2->setStickySessionId("5678");
		
		session1.reset();
		session2.reset();
		EVENTUALLY(5,
			result = pool->getActive() == 0u;
		);
		
		// Test that a request always goes to the process with
		// the given sticky session ID.
		
		options.stickySessionId = "1234";
		session1 = pool->get(options);
		ensure_equals(session1->getPid(), app1_pid);
		session2 = pool2->get(options);
		ensure_equals(session2->getPid(), app1_pid);
		
		session1.reset();
		session2.reset();
		EVENTUALLY(5,
			result = pool->getActive() == 0u;
		);
		
		options.stickySessionId = "5678";
		session1 = pool->get(options);
		ensure_equals(session1->getPid(), app2_pid);
		session2 = pool2->get(options);
		ensure_equals(session2->getPid(), app2_pid);
		
		session1.reset();
		session2.reset();
		EVENTUALLY(5,
			result = pool->getActive() == 0u;
		);
		
		// If there's no process with the given sticky session ID
		// then the normal process selection algorithm is used.
		options.stickySessionId = "???";
		session1 = pool->get(options);
		session2 = pool2->get(options);
		ensure(session1->getPid() != session2->getPid());
	}
	
#endif /* USE_TEMPLATE */
