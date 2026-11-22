#ifndef CXIM_MESSAGEBUBBLE_H
#define CXIM_MESSAGEBUBBLE_H

#include <QLabel>
#include <QObject>

#include <functional>

class QKeyEvent;
class QMouseEvent;

/**
 * @file messagebubble.h
 * @brief 聊天气泡辅助控件(ChatView 内部实现细节, 不对外暴露)
 *
 * BubbleLabel: 文本/图片/文件统一用一种气泡控件承载;
 * 媒体消息把光标设为 pointer 并点击触发"另存为", 因此需要可点击的 QLabel。
 * (QLabel 本身没有 clicked 信号, 这里用鼠标释放事件补上。)
 *
 * 它解决两件事(都靠 refit() 自己算尺寸, 不交给布局协商, 详见 refit() 注释):
 *  1. 宽度: 文本一行放得下就用自然宽(不换行), 超过 maxBubbleWidth(= 聊天可视宽的
 *     3/4)才折行。交给 QLabel 自带的 wordWrap + sizeHint 做不到 —— 它给不出
 *     "按文本长度收缩"的宽度, 气泡会被拉成整行宽, 背景铺满一整行, 看起来根本没有
 *     气泡, 左侧头像那列也跟着被撑宽。
 *  2. 高度: 用 QTextDocument 真排一遍版拿完整行高(含 ascent/descent)。之前用
 *     QFontMetrics::boundingRect 只拿到文字 ink 盒, 少 3~5px → 第一行顶部和
 *     最后一行底部被削平。
 *
 * KeyPressFilter: QTextEdit 的按键过滤器。Qt 中键事件落在滚动区域的
 * viewport 上, 不能直接在 QTextEdit 子类里改 chatview.h 契约, 所以用
 * 独立 QObject 事件过滤器实现 "Enter 发送 / Shift+Enter 换行"。
 */
class BubbleLabel : public QLabel
{
    Q_OBJECT
public:
    explicit BubbleLabel(QWidget *parent = nullptr);

    /// 下发气泡最大宽度(= 聊天可视宽的 3/4); 内部立即按新上限重算并锁死自身尺寸
    void setMaxBubbleWidth(int w);
    int maxBubbleWidth() const { return m_maxW; }

    /**
     * @brief 按当前文本(或缩略图)算出宽高, 然后 setFixedSize 把自己锁死
     *
     * 为什么不走 sizeHint/heightForWidth 的布局协商:
     *   协商结果取决于布局"什么时候问、按哪个宽度问"。实测会出现宽度按 sizeHint 给、
     *   高度却按另一个(更窄的)宽度算出来的不一致, 结果就是文字上下被裁, 而且改算法
     *   也看不到效果 —— 因为决定权在布局手里。setFixedSize 之后布局无从拉伸或压缩,
     *   高度恒等于这里算出的值, 行为完全确定。
     *
     * ⚠ 代价: 文本/图片/上限每次变化后都必须再调一次 refit(), 否则尺寸停留在旧值。
     *   调用点: ChatView::appendMessage、showBubbleMedia(含下载完成的异步回调)、
     *   发送占位行, 以及窗口缩放时的 setMaxBubbleWidth 内部。
     */
    void refit();

signals:
    void clicked();

protected:
    void mouseReleaseEvent(QMouseEvent *e) override;

private:
    /// 是否为图片气泡(setPixmap 过缩略图): 尺寸由图决定, 不参与文字换行计算
    bool isImageBubble() const;

    /// 文本在指定外框宽度下换行后的总高度(已含内边距与上下 1px 边框)
    int textHeightFor(int width) const;

    int m_maxW = 480;
};

class KeyPressFilter : public QObject
{
    Q_OBJECT
public:
    /// handler 返回 true 表示事件已消费(不再传递给 QTextEdit)
    using Handler = std::function<bool(QKeyEvent *)>;

    explicit KeyPressFilter(Handler handler, QObject *parent = nullptr);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    Handler m_handler;
};

#endif // CXIM_MESSAGEBUBBLE_H
