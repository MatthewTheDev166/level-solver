#pragma once

#if __has_include(<legowiifun.cheat_api/include/cheatAPI.hpp>)
    #include <legowiifun.cheat_api/include/cheatAPI.hpp>
#elif __has_include(<cheatAPI.hpp>)
    #include <cheatAPI.hpp>
#else
    #include <Geode/Geode.hpp>
    #include <Geode/loader/Dispatch.hpp>

    namespace cheatAPIEvents {
        inline void setCheatingAll() GEODE_EVENT_EXPORT_NORES(&setCheatingAll, ());
        inline void endCheatingAll() GEODE_EVENT_EXPORT_NORES(&endCheatingAll, ());
    }
#endif
