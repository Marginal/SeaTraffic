include version.mak

TARGET=$(PROJECT)_$(VER).zip

all:	$(TARGET)

clean:
	rm $(TARGET)

$(TARGET):	$(PROJECT)-ReadMe.html $(PROJECT)/buildroutes.py $(PROJECT)/routes.txt $(PROJECT)/lin.xpl $(PROJECT)/mac.xpl $(PROJECT)/win.xpl
	chmod +x $(PROJECT)/*.xpl
	rm -f $(TARGET)
	zip $(TARGET) $+
