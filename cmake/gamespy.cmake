set(GS_OPENSSL FALSE)
set(GAMESPY_SERVER_NAME "server.cnc-online.net")

FetchContent_Declare(
    gamespy
    GIT_REPOSITORY https://github.com/TheSuperHackers/GamespySDK.git
    GIT_TAG        07e3d15c500415abc281efb74322ab6d9c857eb8
)

FetchContent_MakeAvailable(gamespy)

if(VITA)
    # The SDK selects its backend per operating system and knows no generic POSIX target.
    # Its Linux backend is plain BSD sockets and pthreads, which newlib provides on the Vita.
    target_compile_definitions(gscommon PRIVATE __linux__ MAX_PATH=260)
endif()
