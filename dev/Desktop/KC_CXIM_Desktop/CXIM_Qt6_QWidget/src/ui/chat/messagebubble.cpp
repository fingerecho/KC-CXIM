#include "messagebubble.h"

#include <QKeyEvent>
#include <QMouseEvent>
// 尺寸计算: QTextDocument 排版量高 + 字体度量算自然宽度, 显式包含不依赖传递包含
#include <QFontMetrics>
#include <QRect>
#include <QTextDocument>
#include <QTextOption>

BubbleLabel::BubbleLabel(QWidget *parent)
    : QLabel(parent)
{
}

// ---------------------------------------------------------------------------
// 尺寸计算: 文本一行放得下就用自然宽度, 放不下才截到 m_maxW 并换行
// ---------------------------------------------------------------------------
void BubbleLabel::setMaxBubbleWidth(int w)
{
    // 不做"上限没变就直接 return"的优化: refit() 必须在每次下发后被调用一次,
    // 否则新建气泡(文本刚 setText)可能停在默认 480 上限对应的旧尺寸上。
    m_maxW = w;
    refit();
}

// 图片气泡(用 setPixmap 承载缩略图)尺寸由图决定, 不参与文字换行计算。
// ⚠ Qt6 的 QLabel::pixmap() 返回**值类型 QPixmap**(Qt5 时代返回指针), 而且 QLabel
//    并没有 hasPixmap() 这个函数, 所以判空只能写 pixmap().isNull():
//      pixmap()==nullptr / if(pixmap()) / hasPixmap() 三种都编译不过。
bool BubbleLabel::isImageBubble() const
{
    return !pixmap().isNull();
}

int BubbleLabel::textHeightFor(int width) const
{
    // 高度算法踩过两个坑, 记录一下免得再改回去:
    //  ① QFontMetrics::boundingRect → 只给文字 ink 包围盒, 不含首行 ascent 余量与末行
    //     descent, 少 3~5px, 表现为"第一行顶部、最后一行底部被削平"。
    //  ② QLabel::heightForWidth → 内部依赖 isTextLabel 标志, 在 setText 之前/某些
    //     wordWrap 状态下直接返回 -1, 高度退化成一行 → 多行气泡照样被裁。
    // 这里改用 QTextDocument 真排一遍版: 它按 lineSpacing 逐行累计(含 ascent/descent),
    // 与 QLabel wordWrap 的实绘度量同源, 且结果稳定可预期。
    QTextDocument doc;
    doc.setDocumentMargin(0);                    // 自己管内边距, 不要文档默认留白
    QTextOption opt = doc.defaultTextOption();
    opt.setWrapMode(QTextOption::WordWrap);      // 与 QLabel 的 wordWrap 同规则(按词断行)
    doc.setDefaultTextOption(opt);
    doc.setDefaultFont(font());
    doc.setPlainText(text());

    // 内框宽: 外框 - 左右内边距 - 左右 1px 边框。口径必须与 sizeHint() 里的 natural
    // 完全一致(那里也是按这个公式反推外框宽), 否则会出现"外框够宽但按更窄的内框
    // 重排, 结果多算一行高"的自相矛盾。
    const int inner = qMax(20, width - 2 * margin() - 2);
    doc.setTextWidth(inner);

    // doc.size().height() 已含完整的行高(ascender+descender+leading), 不再另加余量,
    // 只补回内边距与上下边框 —— 高度刚好包住文本, 不会空出一截。
    return int(doc.size().height()) + 2 * margin() + 2;
}

void BubbleLabel::refit()
{
    if (isImageBubble()) {
        // 缩略图气泡: QLabel 按 pixmap + margin 给的就是准确尺寸, 照抄后同样锁死
        setFixedSize(QLabel::sizeHint());
        return;
    }

    // 一行放得下就不换行: 外框宽 = 文本宽 + 左右内边距 + 左右 1px 边框 + 取整余量。
    // kWidthSlack 不能省 —— 若给到"与文本严格等宽", 排版引擎会把末尾判成放不下而折
    // 成两行, 短消息凭空多出一行高。
    constexpr int kWidthSlack = 8;
    const int natural = fontMetrics().horizontalAdvance(text())
                        + 2 * margin() + 2 + kWidthSlack;
    const int w = qMin(natural, m_maxW);

    // 宽与高必须出自同一次排版: textHeightFor(w) 的排版内框就是由 w 反推的,
    // 两者口径一致, 所以 setFixedSize 后既不会裁字也不会空出一截。
    setFixedSize(w, textHeightFor(w));
}

void BubbleLabel::mouseReleaseEvent(QMouseEvent *e)
{
    // 左键在控件范围内抬起 → 视为点击(媒体气泡 = 另存为入口)
    if (e->button() == Qt::LeftButton && rect().contains(e->position().toPoint()))
        emit clicked();
    QLabel::mouseReleaseEvent(e);
}

KeyPressFilter::KeyPressFilter(Handler handler, QObject *parent)
    : QObject(parent)
    , m_handler(std::move(handler))
{
}

bool KeyPressFilter::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress && m_handler) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (m_handler(ke))
            return true;    // 已消费(例如 Enter 发送), 不再落到输入框
    }
    return QObject::eventFilter(watched, event);
}
