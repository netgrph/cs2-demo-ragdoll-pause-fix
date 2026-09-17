"""Print a DLL's imported modules, machine, and TimeDateStamp (checks the addon has no runtime DLL dependencies)."""
import sys
import pefile

pe = pefile.PE(sys.argv[1])
print(f"machine {pe.FILE_HEADER.Machine:#x} stamp {pe.FILE_HEADER.TimeDateStamp:#x} size {len(pe.__data__)}")
for imp in pe.DIRECTORY_ENTRY_IMPORT:
    print("import", imp.dll.decode())
