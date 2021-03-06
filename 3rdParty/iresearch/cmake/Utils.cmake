macro(SET_FIND_LIBRARY_OPTIONS _prefixes _suffixes)
  set(_CMAKE_FIND_LIBRARY_PREFIXES "${CMAKE_FIND_LIBRARY_PREFIXES}")
  set(_CMAKE_FIND_LIBRARY_SUFFIXES "${CMAKE_FIND_LIBRARY_SUFFIXES}")
  
  set(CMAKE_FIND_LIBRARY_PREFIXES "${_prefixes}" CACHE INTERNAL "" FORCE )
  set(CMAKE_FIND_LIBRARY_SUFFIXES "${_suffixes}" CACHE INTERNAL "" FORCE ) 
endmacro()

macro(RESTORE_FIND_LIBRARY_OPTIONS)
  set(CMAKE_FIND_LIBRARY_PREFIXES "${_CMAKE_FIND_LIBRARY_PREFIXES}" CACHE INTERNAL "" FORCE)
  set(CMAKE_FIND_LIBRARY_SUFFIXES "${_CMAKE_FIND_LIBRARY_SUFFIXES}" CACHE INTERNAL "" FORCE)
endmacro()
