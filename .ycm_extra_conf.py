def Settings( **kwargs ):
    return {	
            'flags' : [
                '-std=c11',
                '-v',
                '-g',
                '-c',
                '-Weverything',
                # macro defines
                '-D', 'CIMGUI_DEFINE_ENUMS_AND_STRUCTS',
                # includes
				'-include', "C:\\Users\\maxim\\source\\repos\\cnewsetup\\source\\cnewsetup.h",

                # Windows SDK
                '-I', "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.18362.0\\ucrt",
                '-I', "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.18362.0\\um",
                '-I', "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.18362.0\\shared",
                '-I', "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.18362.0\\winrt",
                '-I', "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.18362.0\\cppwinrt",
                '-I', "C:\\Program Files (x86)\\Windows Kits\\NETFXSDK\\4.7.2\\Include\\um",
				'C:\\Users\\maxim\\source\\repos\\cnewsetup\\source\\game_code.c'
                ]
            }
