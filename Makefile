include version.mak

TARGET=$(PROJECT)_$(VER).zip

all:	$(TARGET)

clean:
	rm $(TARGET)

$(TARGET):	$(PROJECT)-ReadMe.html $(PROJECT)/lin.xpl $(PROJECT)/mac.xpl $(PROJECT)/win.xpl $(PROJECT)/buildroutes.py $(PROJECT)/routes.txt $(PROJECT)/enhancedby_opensceneryx_logo.png $(PROJECT)/Osm_linkage.png
	chmod +x $(PROJECT)/*.xpl
	rm -f $(TARGET)
	zip $(TARGET) $+
