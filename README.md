# AbsoluteOS
That the core of the TetraTechnica's programmes/project, it's an OS for arm64 only that can run on every device in theory and with some adaptation. AbsolutOS is made with the minimum requirement to dev/install/use tools to configure/use your devices in arm64.

# Usage case
AbsoluteOS is made for many things, it's why it's a very general OS, like a multi purpose linux, this OS can worl for like Rasperry Pi for your mini arcade station projects to biggest project like the OS that will replace windows.

# Why / Why arm64 only ?
because I'm bored. 
Seriously it's because I like code low level programmes, and because I need a good base for my futures projects,

And why arm64 only ? Because I think arm64 is the future, and many of the usage case of this OS and adaptations of this OS is for arm64 devices like efficient servers, compact/very compact systems.

# How to use it.
For the moment, the OS work in Qemu, with the make.sh that run on macOS and some tool versions are comming for linux and probably windows if someone adapt it for it (sry I hate windows so I'm not gonna code in .bat).
When you run the make.sh, the script detect automaticaly all the files in the differents directory and compile all together to produce the .EFI in /esp, and then the same script run this file.
