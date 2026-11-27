# Helper macros to convert spaces into question marks and back again
e := 
sp := $(e) $(e)
qs = $(subst ?,$(sp),$1)
sq = $(subst $(sp),?,$1)

# Get name of this Makefile (avoid getting word 0 and a starting space)
makefile_name := $(wordlist 1,1000,$(MAKEFILE_LIST))

# Get path of this Makefile
makefile_path := $(call qs,$(dir $(call sq,$(abspath $(call sq,$(makefile_name))))))

# Get path where the Application is
application_path := $(call qs,$(abspath $(call sq,$(makefile_path)../..)))

.PHONY: clean assets all

ifneq ($(words $(makefile_path))$(words $(MAKEFILE_LIST)),11)
all clean assets:
$(error Spaces not allowed in path)
else

ADDITIONAL_SOURCES := 
# The same extra include paths as simulator/msvs/Application.vcxproj
ADDITIONAL_INCLUDE_PATHS := ../../../SDK/Libs/Header ../../Libs/Header ../../ThirdParty/Nayuki
ADDITIONAL_LIBRARY_PATHS := 
ADDITIONAL_LIBRARIES :=
export ADDITIONAL_SOURCES ADDITIONAL_INCLUDE_PATHS ADDITIONAL_LIBRARY_PATHS ADDITIONAL_LIBRARIES

# MinGW only: unoptimised, gui/src/model/Model.cpp instantiates so many
# std::variant helpers that the section names overflow the COFF string table
# ("file too big", even with -Wa,-mbig-obj). -Og inlines most of them and
# keeps the build debuggable. It is added to the user_cflags TouchGFX
# Designer writes in config/gcc/app.mk, through a gcc response file: MinGW
# make splits a command-line value with a space when it passes it on.
ifeq ($(OS),Windows_NT)
-include $(application_path)/config/gcc/app.mk
user_cflags_file := build/user_cflags.rsp
extra_make_vars := user_cflags=@$(user_cflags_file)
endif

all: $(filter assets,$(MAKECMDGOALS))
all assets: $(filter clean,$(MAKECMDGOALS))
all clean assets:
ifeq ($(OS),Windows_NT)
	@mkdir -p "$(application_path)/build" && echo '$(user_cflags) -Og' > "$(application_path)/$(user_cflags_file)"
endif
	@$(MAKE) -r -f generated/simulator/gcc/Makefile -s $(MFLAGS) $(extra_make_vars) $@ -C "$(application_path)"
endif

