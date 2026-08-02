#pragma once

namespace photonstack {

#ifndef PHOTONSTACK_VERSION
#define PHOTONSTACK_VERSION "0.0.0-dev"
#endif

#ifndef PHOTONSTACK_ENGINE_VERSION
#define PHOTONSTACK_ENGINE_VERSION "0.0.0-dev"
#endif

#ifndef PHOTONSTACK_GIT_COMMIT
#define PHOTONSTACK_GIT_COMMIT "unknown"
#endif

#ifndef PHOTONSTACK_BUILD_DATE
#define PHOTONSTACK_BUILD_DATE "unknown"
#endif

inline constexpr const char* kPhotonStackVersion = PHOTONSTACK_VERSION;
inline constexpr const char* kPhotonStackEngineVersion = PHOTONSTACK_ENGINE_VERSION;
inline constexpr const char* kPhotonStackGitCommit = PHOTONSTACK_GIT_COMMIT;
inline constexpr const char* kPhotonStackBuildDate = PHOTONSTACK_BUILD_DATE;

} // namespace photonstack
