// MenuPage.cpp

#include "StdAfx.h"

#include "../Common/ZipRegistry.h"

#include "../../../Windows/DLL.h"
#include "../../../Windows/ErrorMsg.h"
#include "../../../Windows/FileFind.h"

#include "../Explorer/ContextMenuFlags.h"
#include "../Explorer/RegistryContextMenu.h"
#include "../Explorer/resource.h"

#include "../FileManager/PropertyNameRes.h"

#include "../GUI/ExtractDialogRes.h"

#include "FormatUtils.h"
#include "HelpUtils.h"
#include "LangUtils.h"
#include "MenuPage.h"
#include "MenuPageRes.h"

// **************** NanaZip Modification Start ****************
#include <appmodel.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
// **************** NanaZip Modification End ****************

using namespace NWindows;
using namespace NContextMenuFlags;

// **************** NanaZip Modification Start ****************
namespace winrt
{
    using Windows::Foundation::Uri;
    using Windows::System::Launcher;
    using Windows::System::LauncherOptions;
}
// **************** NanaZip Modification End ****************

#ifdef Z7_LANG
static const UInt32 kLangIDs[] =
{
  // **************** NanaZip Modification Start ****************
  // IDX_SYSTEM_INTEGRATE_TO_MENU,
  // IDX_SYSTEM_CASCADED_MENU,
  // IDX_SYSTEM_ICON_IN_MENU,
  IDB_SYSTEM_ASSOCIATE,
  // **************** NanaZip Modification End ****************
  IDX_EXTRACT_ELIM_DUP,
  IDT_SYSTEM_ZONE,
  // **************** NanaZip Modification Start ****************
  IDX_EXTRACT_ON_OPEN,
  // **************** NanaZip Modification End ****************
  IDT_SYSTEM_CONTEXT_MENU_ITEMS
};
#endif

// **************** NanaZip Modification Start ****************
// Removed from NanaZip.
// #define kMenuTopic "fm/options.htm#sevenZip"
// **************** NanaZip Modification End ****************

struct CContextMenuItem
{
  unsigned ControlID;
  UInt32 Flag;
};

static const CContextMenuItem kMenuItems[] =
{
  { IDS_CONTEXT_OPEN, kOpen },
  // **************** NanaZip Modification Start ****************
  // Removed from NanaZip.
  // { IDS_CONTEXT_OPEN, kOpenAs },
  // **************** NanaZip Modification End ****************
  { IDS_CONTEXT_EXTRACT, kExtract },
  { IDS_CONTEXT_EXTRACT_HERE, kExtractHere },
  // **************** NanaZip Modification Start ****************
  { IDS_CONTEXT_EXTRACT_HERE_SMART, kExtractHereSmart },
  // **************** NanaZip Modification End ****************
  { IDS_CONTEXT_EXTRACT_TO, kExtractTo },

  { IDS_CONTEXT_TEST, kTest },

  { IDS_CONTEXT_COMPRESS, kCompress },
  { IDS_CONTEXT_COMPRESS_TO, kCompressTo7z },
  { IDS_CONTEXT_COMPRESS_TO, kCompressToZip },

  #ifndef UNDER_CE
  { IDS_CONTEXT_COMPRESS_EMAIL, kCompressEmail },
  { IDS_CONTEXT_COMPRESS_TO_EMAIL, kCompressTo7zEmail },
  { IDS_CONTEXT_COMPRESS_TO_EMAIL, kCompressToZipEmail },
  #endif

  // **************** NanaZip Modification Start ****************
  // NanaZip doesn't support Cascaded.
  // { IDS_PROP_CHECKSUM, kCRC },
  // { IDS_PROP_CHECKSUM, kCRC_Cascaded },
  { IDS_PROP_CHECKSUM, kCRC }
  // **************** NanaZip Modification End ****************
};


#if !defined(_WIN64)
extern bool g_Is_Wow64;
#endif

#ifndef KEY_WOW64_64KEY
  #define KEY_WOW64_64KEY (0x0100)
#endif

#ifndef KEY_WOW64_32KEY
  #define KEY_WOW64_32KEY (0x0200)
#endif


static void LoadLang_Spec(UString &s, UInt32 id, const char *eng)
{
  LangString(id, s);
  if (s.IsEmpty())
    s = eng;
  s.RemoveChar(L'&');
}


bool CMenuPage::OnInit()
{
  _initMode = true;

  Clear_MenuChanged();

#ifdef Z7_LANG
  LangSetDlgItems(*this, kLangIDs, Z7_ARRAY_SIZE(kLangIDs));
#endif

  // **************** NanaZip Modification Start ****************
  // Removed from NanaZip.
#if 0 // ******** Annotated 7-Zip Mainline Source Code snippet Start ********
  #ifdef UNDER_CE

  HideItem(IDX_SYSTEM_INTEGRATE_TO_MENU);
  HideItem(IDX_SYSTEM_INTEGRATE_TO_MENU_2);

  #else

  {
    UString s;
    {
      CWindow window(GetItem(IDX_SYSTEM_INTEGRATE_TO_MENU));
      window.GetText(s);
    }
    UString bit64 = LangString(IDS_PROP_BIT64);
    if (bit64.IsEmpty())
      bit64 = "64-bit";
    #ifdef _WIN64
      bit64.Replace(L"64", L"32");
    #endif
    s.Add_Space();
    s.Add_Char('(');
    s += bit64;
    s.Add_Char(')');
    SetItemText(IDX_SYSTEM_INTEGRATE_TO_MENU_2, s);
  }

  const FString prefix = NDLL::GetModuleDirPrefix();

  _dlls[0].ctrl = IDX_SYSTEM_INTEGRATE_TO_MENU;
  _dlls[1].ctrl = IDX_SYSTEM_INTEGRATE_TO_MENU_2;

  _dlls[0].wow = 0;
  _dlls[1].wow =
      #ifdef _WIN64
        KEY_WOW64_32KEY
      #else
        KEY_WOW64_64KEY
      #endif
      ;

  for (unsigned d = 0; d < 2; d++)
  {
    CShellDll &dll = _dlls[d];

    dll.wasChanged = false;

    #ifndef _WIN64
    if (d != 0 && !g_Is_Wow64)
    {
      HideItem(dll.ctrl);
      continue;
    }
    #endif

    FString &path = dll.Path;
    path = prefix;
    path += (d == 0 ? "7-zip.dll" :
        #ifdef _WIN64
          "7-zip32.dll"
        #else
          "7-zip64.dll"
        #endif
        );


    if (!NFile::NFind::DoesFileExist_Raw(path))
    {
      path.Empty();
      EnableItem(dll.ctrl, false);
    }
    else
    {
      dll.prevValue = CheckContextMenuHandler(fs2us(path), dll.wow);
      CheckButton(dll.ctrl, dll.prevValue);
    }
  }

  #endif
#endif // ******** Annotated 7-Zip Mainline Source Code snippet End ********
  // **************** NanaZip Modification End ****************


  CContextMenuInfo ci;
  ci.Load();

  // **************** NanaZip Modification Start ****************
  // Removed from NanaZip.
  // CheckButton(IDX_SYSTEM_CASCADED_MENU, ci.Cascaded.Val);
  // CheckButton(IDX_SYSTEM_ICON_IN_MENU, ci.MenuIcons.Val);
  // **************** NanaZip Modification End ****************
  CheckButton(IDX_EXTRACT_ELIM_DUP, ci.ElimDup.Val);

  // **************** NanaZip Modification Start ****************
  CheckButton(IDX_EXTRACT_ON_OPEN, ci.ExtractOnOpen.Val);
  // **************** NanaZip Modification End ****************

  _listView.Attach(GetItem(IDL_SYSTEM_OPTIONS));
  _zoneCombo.Attach(GetItem(IDC_SYSTEM_ZONE));

  {
    unsigned wz = ci.WriteZone;
    if (wz == (UInt32)(Int32)-1)
      // **************** NanaZip Modification Start ****************
      // wz = 0;
      wz = NExtract::NZoneIdMode::Default;
      // **************** NanaZip Modification End ****************
    for (unsigned i = 0; i <= 3; i++)
    {
      unsigned val = i;
      UString s;
      if (i == 3)
      {
        if (wz < 3)
          break;
        val = wz;
      }
      else
      {
        #define MY_IDYES  406
        #define MY_IDNO   407
        if (i == 0)
          LoadLang_Spec(s, MY_IDNO, "No");
        else if (i == 1)
          LoadLang_Spec(s, MY_IDYES, "Yes");
        else
          LangString(IDT_ZONE_FOR_OFFICE, s);
      }
      if (s.IsEmpty())
        s.Add_UInt32(val);
      // **************** NanaZip Modification Start ****************
      // if (i == 0)
      if (i == NExtract::NZoneIdMode::Default)
        s.Insert(0, L"* ");
      // **************** NanaZip Modification End ****************
      const int index = (int)_zoneCombo.AddString(s);
      _zoneCombo.SetItemData(index, (LPARAM)val);
      if (val == wz)
        _zoneCombo.SetCurSel(index);
    }
  }


  const UInt32 newFlags = LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT;
  _listView.SetExtendedListViewStyle(newFlags, newFlags);

  _listView.InsertColumn(0, L"", 200);

  for (unsigned i = 0; i < Z7_ARRAY_SIZE(kMenuItems); i++)
  {
    const CContextMenuItem &menuItem = kMenuItems[i];

    UString s = LangString(menuItem.ControlID);
    // **************** NanaZip Modification Start ****************
#if 0 // ******** Annotated 7-Zip Mainline Source Code snippet Start ********
    if (menuItem.Flag == kCRC)
      s = "CRC SHA";
    else if (menuItem.Flag == kCRC_Cascaded)
      s = "7-Zip > CRC SHA";
    if (menuItem.Flag == kOpenAs
        || menuItem.Flag == kCRC
        || menuItem.Flag == kCRC_Cascaded)
      s += " >";
#endif // ******** Annotated 7-Zip Mainline Source Code snippet End ********
    if (menuItem.Flag == kCRC)
      s = "HASH";
    // **************** NanaZip Modification End ****************

    switch (menuItem.ControlID)
    {
      case IDS_CONTEXT_EXTRACT_TO:
      {
        s = MyFormatNew(s, LangString(IDS_CONTEXT_FOLDER));
        break;
      }
      case IDS_CONTEXT_COMPRESS_TO:
      case IDS_CONTEXT_COMPRESS_TO_EMAIL:
      {
        UString s2 = LangString(IDS_CONTEXT_ARCHIVE);
        switch (menuItem.Flag)
        {
          case kCompressTo7z:
          case kCompressTo7zEmail:
            s2 += (".7z");
            break;
          case kCompressToZip:
          case kCompressToZipEmail:
            s2 += (".zip");
            break;
        }
        s = MyFormatNew(s, s2);
        break;
      }
    }

    const int itemIndex = _listView.InsertItem(i, s);
    _listView.SetCheckState((unsigned)itemIndex, ((ci.Flags & menuItem.Flag) != 0));
  }

  _listView.SetColumnWidthAuto(0);
  _initMode = false;

  return CPropertyPage::OnInit();
}


// **************** NanaZip Modification Start ****************
// Removed from NanaZip.
#if 0 // ******** Annotated 7-Zip Mainline Source Code snippet Start ********
#ifndef UNDER_CE

static void ShowMenuErrorMessage(const wchar_t *m, HWND hwnd)
{
  MessageBoxW(hwnd, m, L"7-Zip", MB_ICONERROR);
}

#endif
#endif // ******** Annotated 7-Zip Mainline Source Code snippet End ********
// **************** NanaZip Modification End ****************


LONG CMenuPage::OnApply()
{
  // **************** NanaZip Modification Start ****************
  // Removed from NanaZip.
#if 0 // ******** Annotated 7-Zip Mainline Source Code snippet Start ********
  #ifndef UNDER_CE

  for (unsigned d = 2; d != 0;)
  {
    d--;
    CShellDll &dll = _dlls[d];
    if (dll.wasChanged && !dll.Path.IsEmpty())
    {
      const bool newVal = IsButtonCheckedBool(dll.ctrl);
      const LONG res = SetContextMenuHandler(newVal, fs2us(dll.Path), dll.wow);
      if (res != ERROR_SUCCESS && (dll.prevValue != newVal || newVal))
        ShowMenuErrorMessage(NError::MyFormatMessage(res), *this);
      dll.prevValue = CheckContextMenuHandler(fs2us(dll.Path), dll.wow);
      CheckButton(dll.ctrl, dll.prevValue);
      dll.wasChanged = false;
    }
  }

  #endif
#endif // ******** Annotated 7-Zip Mainline Source Code snippet End ********
  // **************** NanaZip Modification End ****************

  // **************** NanaZip Modification Start ****************
  // NanaZip doesn't support Cascaded.
#if 0 // ******** Annotated 7-Zip Mainline Source Code snippet Start ********
  if (_cascaded_Changed
      || _menuIcons_Changed
      || _elimDup_Changed
      || _writeZone_Changed
      || _flags_Changed)
#endif // ******** Annotated 7-Zip Mainline Source Code snippet End ********
  if (_elimDup_Changed
      || _writeZone_Changed
      || _flags_Changed
      || m_ExtractOnOpenChanged)
  // **************** NanaZip Modification End ****************
  {
    CContextMenuInfo ci;
    // **************** NanaZip Modification Start ****************
    // Removed from NanaZip.
#if 0 // ******** Annotated 7-Zip Mainline Source Code snippet Start ********
    ci.Cascaded.Val = IsButtonCheckedBool(IDX_SYSTEM_CASCADED_MENU);
    ci.Cascaded.Def = _cascaded_Changed;

    ci.MenuIcons.Val = IsButtonCheckedBool(IDX_SYSTEM_ICON_IN_MENU);
    ci.MenuIcons.Def = _menuIcons_Changed;
#endif // ******** Annotated 7-Zip Mainline Source Code snippet End ********
    // **************** NanaZip Modification End ****************

    ci.ElimDup.Val = IsButtonCheckedBool(IDX_EXTRACT_ELIM_DUP);
    ci.ElimDup.Def = _elimDup_Changed;

    {
      int zoneIndex = (int)_zoneCombo.GetItemData_of_CurSel();
      if (zoneIndex <= 0)
        zoneIndex = -1;
      ci.WriteZone = (UInt32)(Int32)zoneIndex;
    }

    ci.Flags = 0;

    for (unsigned i = 0; i < Z7_ARRAY_SIZE(kMenuItems); i++)
      if (_listView.GetCheckState(i))
        ci.Flags |= kMenuItems[i].Flag;

    ci.Flags_Def = _flags_Changed;

    // **************** NanaZip Modification Start ****************
    ci.ExtractOnOpen.Val = IsButtonCheckedBool(IDX_EXTRACT_ON_OPEN);
    ci.ExtractOnOpen.Def = m_ExtractOnOpenChanged;
    // **************** NanaZip Modification End ****************

    ci.Save();

    Clear_MenuChanged();
  }

  // UnChanged();

  return PSNRET_NOERROR;
}

// **************** NanaZip Modification Start ****************
// Removed from NanaZip.
#if 0 // ******** Annotated 7-Zip Mainline Source Code snippet Start ********
void CMenuPage::OnNotifyHelp()
{
  ShowHelpWindow(kMenuTopic);
}
#endif // ******** Annotated 7-Zip Mainline Source Code snippet End ********
// **************** NanaZip Modification End ****************

bool CMenuPage::OnButtonClicked(unsigned buttonID, HWND buttonHWND)
{
  switch (buttonID)
  {
    // **************** NanaZip Modification Start ****************
    // Removed from NanaZip.
#if 0 // ******** Annotated 7-Zip Mainline Source Code snippet Start ********
    #ifndef UNDER_CE
    case IDX_SYSTEM_INTEGRATE_TO_MENU:
    case IDX_SYSTEM_INTEGRATE_TO_MENU_2:
    {
      for (unsigned d = 0; d < 2; d++)
      {
        CShellDll &dll = _dlls[d];
        if (buttonID == dll.ctrl && !dll.Path.IsEmpty())
          dll.wasChanged = true;
      }
      break;
    }
    #endif

    case IDX_SYSTEM_CASCADED_MENU: _cascaded_Changed = true; break;
    case IDX_SYSTEM_ICON_IN_MENU: _menuIcons_Changed = true; break;
#endif // ******** Annotated 7-Zip Mainline Source Code snippet End ********
    // **************** NanaZip Modification End ****************
    case IDX_EXTRACT_ELIM_DUP: _elimDup_Changed = true; break;
    // case IDX_EXTRACT_WRITE_ZONE: _writeZone_Changed = true; break;

    // **************** NanaZip Modification Start ****************
    case IDB_SYSTEM_ASSOCIATE:
    {
        try
        {
            // GetCurrentApplicationUserModelId can't work properly when the
            // program starts from Visual Studio, because NanaZip will be
            // allocated a wrong AUMID.
            UINT32 nLength = 0;
            ::GetCurrentApplicationUserModelId(
                &nLength,
                nullptr);
            std::wstring AUMID(
                nLength,
                '\0');
            winrt::check_hresult(::GetCurrentApplicationUserModelId(
                &nLength,
                AUMID.data()));
            winrt::Uri NavigateURI(
                L"ms-settings:defaultapps?registeredAUMID="
                + AUMID);
            winrt::LauncherOptions Options;
            Options.TargetApplicationPackageFamilyName(
                L"windows.immersivecontrolpanel_cw5n1h2txyewy");
            winrt::Launcher::LaunchUriAsync(
                NavigateURI,
                Options).get();
            return true;
        }
        catch (...)
        {
            return false;
        }
    }
    // **************** NanaZip Modification End ****************

    default:
      return CPropertyPage::OnButtonClicked(buttonID, buttonHWND);
  }

  Changed();
  return true;
}


bool CMenuPage::OnCommand(unsigned code, unsigned itemID, LPARAM param)
{
  if (code == CBN_SELCHANGE && itemID == IDC_SYSTEM_ZONE)
  {
    _writeZone_Changed = true;
    Changed();
    return true;
  }
  return CPropertyPage::OnCommand(code, itemID, param);
}


bool CMenuPage::OnNotify(UINT controlID, LPNMHDR lParam)
{
  if (lParam->hwndFrom == HWND(_listView))
  {
    switch (lParam->code)
    {
      case (LVN_ITEMCHANGED):
        return OnItemChanged((const NMLISTVIEW *)lParam);
    }
  }
  return CPropertyPage::OnNotify(controlID, lParam);
}


bool CMenuPage::OnItemChanged(const NMLISTVIEW *info)
{
  if (_initMode)
    return true;
  if ((info->uChanged & LVIF_STATE) != 0)
  {
    UINT oldState = info->uOldState & LVIS_STATEIMAGEMASK;
    UINT newState = info->uNewState & LVIS_STATEIMAGEMASK;
    if (oldState != newState)
    {
      _flags_Changed = true;
      Changed();
    }
  }
  return true;
}
