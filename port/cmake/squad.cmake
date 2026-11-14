# Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
# Squad Strike idea: Chris's friend Tyler (Squad Strike Gecko codes).
option(MELEE_SQUAD_STRIKE "Build Squad Strike for offline VS and Friends" OFF)
function(mu_squad_target target)
  if(MELEE_SQUAD_STRIKE)
    # PUBLIC: runtime sources AND every consumer of their guarded class layouts agree.
    target_compile_definitions(${target} PUBLIC MU_SQUAD=1)
  endif()
endfunction()
