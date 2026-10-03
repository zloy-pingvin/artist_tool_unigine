#include "IconBrowserPage.h"
#include "UiHelpers.h"

#include <UnigineEngine.h>

#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMetaEnum>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

namespace ArtistTool
{

namespace
{

const int PATH_ROLE = Qt::UserRole;
const int SOURCE_ROLE = Qt::UserRole + 1;
const int DETAILS_ROLE = Qt::UserRole + 2;

// The editor has no image format plugins, so only the formats built into Qt load.
QStringList image_name_filters()
{
	return {QString::fromUtf8("*.png"), QString::fromUtf8("*.bmp"), QString::fromUtf8("*.xpm"),
		QString::fromUtf8("*.ppm")};
}

// Every kind of image the editor may keep in its resources. Which of them load depends
// on the image readers the editor has, see reload().
QStringList resource_name_filters()
{
	return {QString::fromUtf8("*.png"), QString::fromUtf8("*.bmp"), QString::fromUtf8("*.xpm"),
		QString::fromUtf8("*.ppm"), QString::fromUtf8("*.svg"), QString::fromUtf8("*.gif"),
		QString::fromUtf8("*.jpg"), QString::fromUtf8("*.jpeg"), QString::fromUtf8("*.ico"),
		QString::fromUtf8("*.webp")};
}

// Stands in the list for an image the plugin can not load: a box with its format.
QIcon placeholder_icon(const QString &format)
{
	QPixmap pixmap(64, 64);
	pixmap.fill(Qt::transparent);

	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(QPen(QColor(150, 150, 150), 2, Qt::DashLine));
	painter.setBrush(QColor(60, 62, 66));
	painter.drawRoundedRect(QRectF(4, 4, 56, 56), 6, 6);

	QFont font = painter.font();
	font.setPixelSize(20);
	font.setBold(true);
	painter.setFont(font);
	painter.setPen(QColor(200, 200, 200));
	painter.drawText(pixmap.rect(), Qt::AlignCenter, format.toUpper());
	return QIcon(pixmap);
}

QString size_text(const QSize &size)
{
	return QString::fromUtf8("%1 × %2 px").arg(size.width()).arg(size.height());
}

} // namespace

IconBrowserPage::IconBrowserPage(QWidget *parent)
	: QWidget(parent)
{
	QVBoxLayout *layout = new QVBoxLayout(this);

	QLabel *description = new QLabel(loc_.tip(
		"Icons a tool can use: the ones built into the editor, the Qt standard ones and your "
		"own PNG files from the plugin icons folder. Click an icon to see its path - a tool "
		"loads the icon by this path. Double-click copies the path.",
		"Иконки, которые может использовать инструмент: встроенные в редактор, стандартные "
		"иконки Qt и ваши собственные PNG из папки icons плагина. Клик по иконке показывает "
		"её путь - по этому пути инструмент её и подключает. Двойной клик копирует путь."));
	description->setWordWrap(true);
	layout->addWidget(description);

	QHBoxLayout *filters = new QHBoxLayout;
	source_ = new QComboBox;
	source_->addItem(QString::fromUtf8("All"), SOURCE_ALL);
	source_->addItem(QString::fromUtf8("Editor"), SOURCE_EDITOR);
	source_->addItem(QString::fromUtf8("Qt standard"), SOURCE_QT);
	source_->addItem(QString::fromUtf8("My icons"), SOURCE_PLUGIN);
	source_->setToolTip(loc_.tip(
		"Editor - icons built into the UNIGINE Editor.\n"
		"Qt standard - icons of the Qt library (folders, files, arrows, dialog buttons).\n"
		"My icons - PNG files from the plugin icons folder.",
		"Editor - иконки, встроенные в UNIGINE Editor.\n"
		"Qt standard - иконки библиотеки Qt (папки, файлы, стрелки, кнопки диалогов).\n"
		"My icons - PNG-файлы из папки icons плагина."));
	filters->addWidget(source_);

	filter_ = new QLineEdit;
	filter_->setPlaceholderText(QString::fromUtf8("Search: mesh, folder, light..."));
	filter_->setClearButtonEnabled(true);
	filters->addWidget(filter_, 1);
	layout->addLayout(filters);

	QHBoxLayout *size_row = new QHBoxLayout;
	size_row->addWidget(new QLabel(QString::fromUtf8("Size")));
	size_ = new QSlider(Qt::Horizontal);
	size_->setRange(16, 96);
	size_->setSingleStep(8);
	size_->setPageStep(16);
	size_->setValue(32);
	size_->setToolTip(loc_.tip(
		"Size of the icons in the list. Small source images get blurry when made bigger.",
		"Размер иконок в списке. Маленькие исходные картинки при увеличении становятся мыльными."));
	size_row->addWidget(size_, 1);
	count_label_ = new QLabel;
	size_row->addWidget(count_label_);
	layout->addLayout(size_row);

	list_ = new QListWidget;
	list_->setViewMode(QListView::IconMode);
	list_->setResizeMode(QListView::Adjust);
	list_->setMovement(QListView::Static);
	list_->setUniformItemSizes(true);
	list_->setTextElideMode(Qt::ElideMiddle);
	list_->setSelectionMode(QAbstractItemView::SingleSelection);
	layout->addWidget(list_, 1);

	info_label_ = new QLabel;
	info_label_->setWordWrap(true);
	info_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	layout->addWidget(info_label_);

	QHBoxLayout *buttons = new QHBoxLayout;
	copy_button_ = new QPushButton(QString::fromUtf8("Copy path"));
	copy_button_->setToolTip(loc_.tip(
		"Copy the path of the selected icon, to tell which icon a tool should use.",
		"Скопировать путь выбранной иконки - чтобы сказать, какую иконку ставить в инструмент."));
	buttons->addWidget(copy_button_);
	buttons->addStretch(1);

	QPushButton *open_folder = new QPushButton(QString::fromUtf8("Open icons folder"));
	open_folder->setToolTip(loc_.tip(
		"Open the folder for your own icons. Put PNG files there (64 × 64 with transparency "
		"is a good size) and press Reload.",
		"Открыть папку для своих иконок. Положите туда PNG (хороший размер - 64 × 64, с "
		"прозрачностью) и нажмите Reload."));
	buttons->addWidget(open_folder);

	QPushButton *reload_button = new QPushButton(QString::fromUtf8("Reload"));
	reload_button->setToolTip(loc_.tip(
		"Read the icons again, e.g. after adding files to the icons folder.",
		"Перечитать иконки, например после добавления файлов в папку icons."));
	buttons->addWidget(reload_button);
	layout->addLayout(buttons);

	connect(source_, &QComboBox::currentIndexChanged, this, [this](int) { apply_filter(); });
	connect(filter_, &QLineEdit::textChanged, this, [this](const QString &) { apply_filter(); });
	connect(size_, &QSlider::valueChanged, this, [this](int) { apply_icon_size(); });
	connect(list_, &QListWidget::currentItemChanged, this, [this]() { update_info(); });
	connect(list_, &QListWidget::itemDoubleClicked, this, [this]() { copy_button_->click(); });
	connect(copy_button_, &QPushButton::clicked, this, [this]() {
		if (QListWidgetItem *item = list_->currentItem())
		{
			QGuiApplication::clipboard()->setText(item->data(PATH_ROLE).toString());
			update_info();
			info_label_->setText(info_label_->text() + QString::fromUtf8("\nCopied."));
		}
	});
	connect(open_folder, &QPushButton::clicked, this, []() {
		const QString dir = pluginIconsDir();
		QDir().mkpath(dir);
		QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
	});
	connect(reload_button, &QPushButton::clicked, this, [this]() { reload(); });

	apply_icon_size();
	update_info();
}

QString IconBrowserPage::pluginIconsDir()
{
	return ArtistTool::pluginIconsDir();
}

void IconBrowserPage::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	if (!loaded_)
	{
		loaded_ = true;
		reload();
	}
}

void IconBrowserPage::reload()
{
	list_->clear();

	// Icons built into the editor and into Qt itself (Qt keeps its own under qt-project.org).
	// Every image of the resources is listed, so that nothing the editor has is left
	// out. The few the plugin can not load (the editor has no readers for SVG and GIF)
	// are shown as a box with the format and are marked as not usable.
	QStringList resources;
	QDirIterator it(QString::fromUtf8(":/"), resource_name_filters(), QDir::Files, QDirIterator::Subdirectories);
	while (it.hasNext())
		resources.append(it.next());
	resources.sort(Qt::CaseInsensitive);
	for (const QString &path : resources)
	{
		const Source source = path.startsWith(QString::fromUtf8(":/qt-project.org")) ? SOURCE_QT : SOURCE_EDITOR;
		const QFileInfo info(path);

		QImageReader reader(path);
		if (reader.canRead())
		{
			add_item(QIcon(path), info.completeBaseName(), path, source, size_text(reader.size()));
			continue;
		}

		const QString format = info.suffix().toLower();
		add_item(placeholder_icon(format), QString::fromUtf8("%1 (%2)").arg(info.completeBaseName(), format),
			path, source, loc_.tip(
				"Can not be used by a tool: the editor has no reader for this image format.",
				"Инструмент не может её использовать: в редакторе нет загрузчика для этого формата."));
	}

	// Qt standard icons of the current style.
	const QMetaEnum pixmaps = QMetaEnum::fromType<QStyle::StandardPixmap>();
	for (int i = 0; i < pixmaps.keyCount(); ++i)
	{
		const int value = pixmaps.value(i);
		if (value < 0) // SP_CustomBase, not an icon
			continue;
		const QIcon icon = style()->standardIcon(QStyle::StandardPixmap(value));
		if (icon.isNull())
			continue;
		const QString name = QString::fromUtf8(pixmaps.key(i));
		add_item(icon, name.mid(3), QString::fromUtf8("QStyle::") + name, SOURCE_QT,
			QString::fromUtf8("style()->standardIcon(QStyle::%1)").arg(name));
	}

	// The user's own icons.
	const QFileInfoList files = QDir(pluginIconsDir()).entryInfoList(image_name_filters(), QDir::Files, QDir::Name);
	for (const QFileInfo &file : files)
	{
		const QString path = file.absoluteFilePath();
		add_item(QIcon(path), file.completeBaseName(), path, SOURCE_PLUGIN,
			size_text(QImageReader(path).size()));
	}

	update_info();
	apply_filter();
}

void IconBrowserPage::add_item(const QIcon &icon, const QString &name, const QString &path, Source source,
	const QString &details)
{
	QListWidgetItem *item = new QListWidgetItem(icon, name, list_);
	item->setData(PATH_ROLE, path);
	item->setData(SOURCE_ROLE, int(source));
	item->setData(DETAILS_ROLE, details);
	item->setToolTip(path + QString::fromUtf8("\n") + details);
}

void IconBrowserPage::apply_filter()
{
	const int source = source_->currentData().toInt();
	const QString text = filter_->text().trimmed();

	int shown = 0;
	for (int i = 0; i < list_->count(); ++i)
	{
		QListWidgetItem *item = list_->item(i);
		const bool source_ok = source == SOURCE_ALL || item->data(SOURCE_ROLE).toInt() == source;
		const bool text_ok = text.isEmpty() || item->data(PATH_ROLE).toString().contains(text, Qt::CaseInsensitive);
		item->setHidden(!(source_ok && text_ok));
		if (source_ok && text_ok)
			++shown;
	}
	count_label_->setText(QString::fromUtf8("%1 of %2").arg(shown).arg(list_->count()));

	if (source == SOURCE_PLUGIN && shown == 0 && text.isEmpty())
	{
		info_label_->setText(loc_.tip(
			"No own icons yet. Press Open icons folder, put PNG files there and press Reload.",
			"Своих иконок пока нет. Нажмите Open icons folder, положите туда PNG и нажмите Reload."));
	}
}

void IconBrowserPage::apply_icon_size()
{
	const int size = size_->value();
	list_->setIconSize(QSize(size, size));
	list_->setGridSize(QSize(qMax(size + 24, 88), size + 30));
}

void IconBrowserPage::update_info()
{
	QListWidgetItem *item = list_->currentItem();
	copy_button_->setEnabled(item != nullptr);
	if (!item)
	{
		info_label_->setText(loc_.tip(
			"Click an icon to see its path.",
			"Кликните по иконке, чтобы увидеть её путь."));
		return;
	}
	info_label_->setText(item->data(PATH_ROLE).toString() + QString::fromUtf8("\n")
		+ item->data(DETAILS_ROLE).toString());
}

} // namespace ArtistTool
