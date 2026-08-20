# Per-generator packaging settings.
#
# CPack includes this once per generator, with CPACK_GENERATOR set to the one
# being run, which is the only place a setting can differ between the MSI and
# the ZIP. Everything they agree on lives in CMakeLists.txt.

if(CPACK_GENERATOR MATCHES "WIX")
    # No folder named after the version inside the install directory.
    #
    # An archive wants one -- unzipping nine loose files into somebody's
    # Downloads folder is rude -- but an installer already has a directory of its
    # own, and nesting a versioned one inside it puts the program at
    # C:\Program Files\nescc_emu\nescc_emu-1.2.0-windows-x64\. That path then
    # changes with every release, so each upgrade moves the program somewhere
    # new and leaves shortcuts pointing at where it used to be.
    #
    # The two generators want opposite things here, which is what this file is
    # for: the ZIP keeps the folder, the MSI does not.
    set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY 0)
endif()
