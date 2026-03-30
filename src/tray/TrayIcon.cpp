// TrayIcon.cpp - System tray icon implementation
using namespace System;
using namespace System::Windows::Forms;
using namespace System::Drawing;

ref class TrayIcon {
private:
    NotifyIcon^ notifyIcon;
    ContextMenuStrip^ contextMenu;
    MainWindow^ mainWindow;

public:
    TrayIcon(MainWindow^ window) {
        mainWindow = window;
        
        // Create notify icon
        notifyIcon = gcnew NotifyIcon();
        notifyIcon->Text = "KVM Remote Control";
        // Use embedded system icon; replace with a branded KVM.ico when available
        notifyIcon->Icon = System::Drawing::SystemIcons::Application;
        notifyIcon->Visible = true;
        
        // Create context menu
        contextMenu = gcnew ContextMenuStrip();
        
        ToolStripMenuItem^ showItem = gcnew ToolStripMenuItem("Show Control Panel");
        showItem->Click += gcnew EventHandler(this, &TrayIcon::Show_Click);
        contextMenu->Items->Add(showItem);
        
        contextMenu->Items->Add(gcnew ToolStripSeparator());
        
        ToolStripMenuItem^ driversItem = gcnew ToolStripMenuItem("Drivers");
        driversItem->DropDownItems->Add("Keyboard: Running", nullptr, nullptr);
        driversItem->DropDownItems->Add("Mouse: Running", nullptr, nullptr);
        contextMenu->Items->Add(driversItem);
        
        contextMenu->Items->Add(gcnew ToolStripSeparator());
        
        ToolStripMenuItem^ exitItem = gcnew ToolStripMenuItem("Exit");
        exitItem->Click += gcnew EventHandler(this, &TrayIcon::Exit_Click);
        contextMenu->Items->Add(exitItem);
        
        notifyIcon->ContextMenuStrip = contextMenu;
        notifyIcon->DoubleClick += gcnew EventHandler(this, &TrayIcon::Show_Click);
    }
    
    void ShowBalloon(String^ title, String^ text, ToolTipIcon icon) {
        notifyIcon->ShowBalloonTip(3000, title, text, icon);
    }
    
    void Dispose() {
        notifyIcon->Visible = false;
        delete notifyIcon;
    }

private:
    void Show_Click(Object^ sender, EventArgs^ e) {
        mainWindow->Show();
        mainWindow->WindowState = System::Windows::WindowState::Normal;
        mainWindow->Activate();
    }
    
    void Exit_Click(Object^ sender, EventArgs^ e) {
        Application::Exit();
    }
};
