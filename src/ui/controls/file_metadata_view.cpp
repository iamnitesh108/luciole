#include "ui/controls/file_metadata_view.h"

#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/style.h"

#include <algorithm>

FileMetadataView::FileMetadataView() {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(Style::spaceXs);
  const auto literal = [](Label& label) { label.setUseMarkup(false); };
  for (std::size_t i = 0; i < 32; ++i) {
    Row row{};
    addChild(
        ui::row(
            {.out = &row.container,
             .align = FlexAlign::Start,
             .gap = Style::spaceSm,
             .visible = false,
             .participatesInLayout = false},
            ui::label(
                {.out = &row.key,
                 .fontSize = Style::fontSizeCaption,
                 .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                 .maxLines = 2,
                 .configure = literal}
            ),
            ui::label(
                {.out = &row.value,
                 .fontSize = Style::fontSizeCaption,
                 .color = colorSpecFromRole(ColorRole::OnSurface),
                 .maxLines = 3,
                 .flexGrow = 1,
                 .configure = literal}
            )
        )
    );
    m_rows.push_back(row);
  }
  setVisible(false);
  setParticipatesInLayout(false);
}

void FileMetadataView::sync(std::span<const luciole::MetadataField> fields, float width, bool previewOnly) {
  const float keyWidth = std::min(88.0F, width * 0.36F);
  std::size_t count = 0;
  for (const auto& field : fields) {
    if (previewOnly && !field.preview) {
      continue;
    }
    if (count == m_rows.size()) {
      break;
    }
    auto& row = m_rows[count++];
    row.container->setVisible(true);
    row.container->setParticipatesInLayout(true);
    row.key->setText(field.label);
    row.key->setMinWidth(keyWidth);
    row.key->setMaxWidth(keyWidth);
    row.value->setText(field.value);
    row.value->setMaxLines(field.singleLine ? 1 : 3);
    row.value->setEllipsize(field.singleLine ? TextEllipsize::Middle : TextEllipsize::End);
    row.value->setMaxWidth(std::max(1.0F, width - keyWidth - Style::spaceSm));
  }
  for (std::size_t i = count; i < m_rows.size(); ++i) {
    m_rows[i].container->setVisible(false);
    m_rows[i].container->setParticipatesInLayout(false);
  }
  setVisible(count > 0);
  setParticipatesInLayout(count > 0);
}
