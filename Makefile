include version.mak

TARGET=$(PROJECT)_$(VER).zip

FILES=$(PROJECT)-ReadMe.html $(PROJECT)/lin.xpl $(PROJECT)/mac.xpl $(PROJECT)/win.xpl $(PROJECT)/64/lin.xpl $(PROJECT)/64/win.xpl $(PROJECT)/buildroutes.py $(PROJECT)/routes.txt $(PROJECT)/enhancedby_opensceneryx_logo.png $(PROJECT)/Osm_linkage.png $(PROJECT)/Tanker.dds $(PROJECT)/Tanker_LIT.dds $(PROJECT)/Aframax_tanker_Black.obj $(PROJECT)/Aframax_tanker_Blue.obj $(PROJECT)/Aframax_tanker_Grey.obj $(PROJECT)/Aframax_tanker_Sky.obj $(PROJECT)/Damen_4212.dds $(PROJECT)/Damen_4212_Blue.obj $(PROJECT)/Damen_4212_Green.obj $(PROJECT)/Damen_4212_Orange.obj $(PROJECT)/Damen_4212_Sky.obj $(PROJECT)/wake.dds $(PROJECT)/wake_big.obj $(PROJECT)/wake_med.obj

all:	$(TARGET)

clean:
	rm $(TARGET)

$(TARGET):	$(FILES)
	chmod +x $(PROJECT)/*.xpl $(PROJECT)/64/*.xpl
	rm -f $(TARGET)
	zip -MM $(TARGET) $+
