#pragma once

#include "browser/action_registry.h"
#include "fs/location.h"
#include "operations/create_service.h"
#include "operations/template_catalog.h"

#include <memory>
#include <optional>
#include <string>

class LinkDialog;
class NameDialog;
class OperationJob;
class PageContext;
class SubmenuController;

namespace luciole {
  struct ClipboardImage;
} // namespace luciole

// New items: the New Document submenu of the file menu, the create file/folder dialog, the pasted image dialog and the
// link dialog.
class CreateController {
public:
  CreateController(PageContext& page, luciole::TemplateCatalog& templates, SubmenuController& submenus);

  void openCreateItem(
      luciole::Location parent, luciole::CreateKind kind, std::optional<luciole::DocumentTemplate> source = std::nullopt
  );
  void openLinkDialog(const luciole::DirectoryEntry& target);
  // Pastes a clipboard image: asks for a name, then writes the bytes through the creation job.
  void openImagePaste(luciole::ClipboardImage image, luciole::Location parent);
  // Fills the opening New Document submenu with the templates, which load meanwhile.
  void openDocumentMenu(const luciole::ActionRequest& request);
  // The New Document submenu closed.
  void documentMenuClosed() { m_templates.cancel(); }
  // Owned creation job change: sync the dialog on the next frame, or report a job whose dialog closed.
  void changed(OperationJob& job);
  // Frame slot in BrowserPage::prepareFrame().
  void onFrame();

private:
  bool canCreateItem() const;
  void showDocumentMenu(const luciole::ActionRequest& request, luciole::TemplateListing listing, bool loading = false);
  void submitCreateItem(std::string name);
  void sync(const OperationJob& job);
  void closeCreation();

  PageContext& m_page;
  // The create or link dialog's job: checks the destination, then creates the item.
  std::shared_ptr<OperationJob> m_job;
  luciole::TemplateCatalog& m_templates;
  SubmenuController& m_submenus;
  NameDialog* m_createEditor = nullptr;
  LinkDialog* m_linkDialog = nullptr;
  bool m_linkPreparing = false;
  std::optional<luciole::Location> m_createParent;
  bool m_creationPreparing = false;
  bool m_dirty = false;
};
