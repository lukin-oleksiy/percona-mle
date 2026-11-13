# CPack project config (read by cpack for each generator): the SELinux policy package is
# for RPM-based systems only.
IF(CPACK_GENERATOR STREQUAL "DEB")
  LIST(REMOVE_ITEM CPACK_COMPONENTS_ALL MleSelinux)
ENDIF()
