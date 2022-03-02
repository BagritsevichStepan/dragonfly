// Copyright 2022, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "base/init.h"
#include "facade/dragonfly_listener.h"
#include "server/main_service.h"
#include "util/accept_server.h"
#include "util/uring/uring_pool.h"
#include "util/varz.h"

DECLARE_uint32(port);
DECLARE_uint32(memcache_port);

using namespace util;
using namespace std;
using namespace facade;

namespace dfly {

void RunEngine(ProactorPool* pool, AcceptServer* acceptor) {
  Service service(pool);

  service.Init(acceptor);
  acceptor->AddListener(FLAGS_port, new Listener{Protocol::REDIS, &service});
  if (FLAGS_memcache_port > 0) {
    acceptor->AddListener(FLAGS_memcache_port, new Listener{Protocol::MEMCACHE, &service});
  }

  acceptor->Run();
  acceptor->Wait();

  service.Shutdown();
}

}  // namespace dfly

int main(int argc, char* argv[]) {
  gflags::SetUsageMessage("dragonfly [FLAGS]");
  gflags::SetVersionString("v0.2");

  MainInitGuard guard(&argc, &argv);

  CHECK_GT(FLAGS_port, 0u);

  uring::UringPool pp{1024};
  pp.Run();

  AcceptServer acceptor(&pp);

  dfly::RunEngine(&pp, &acceptor);

  pp.Stop();

  return 0;
}
