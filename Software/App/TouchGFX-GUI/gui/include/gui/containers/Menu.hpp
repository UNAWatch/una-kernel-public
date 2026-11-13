#ifndef MENU_HPP
#define MENU_HPP

#include <gui_generated/containers/MenuBase.hpp>

class Menu : public MenuBase
{
public:
    Menu();
    virtual ~Menu() {}

    virtual void initialize();

    static const uint16_t skMaxNumberOfItems = 30;

    virtual void invalidate();

    void setNumberOfItems(int16_t numberOfItems);
    int16_t getNumberOfItems();

    void setTitle(TypedTextId msgId);
    void showTitle(bool state);

    void setBackground(BitmapId iconId);
    void showBackground(bool state);

    // Show/hide the scrollable list parts (wheel + side bar + selection
    // background) so a screen can overlay its own content (e.g. a confirm /
    // result panel) while keeping the title and button hints in place.
    void showListContent(bool state);

    void setInfoMsg(TypedTextId msgId);
    void setInfoMsg(uint16_t count, TypedTextId msgId);
    void setInfoMsgColor(touchgfx::colortype color);

    Buttons &getButtons();
    
    void selectNext();
    void selectPrev();
    void selectItem(int16_t itemIndex);
    // Jump to an item with no scroll animation, including single-item lists
    // (selectItem ignores those, which leaves the wheel scrolling in on entry).
    void snapToItem(int16_t itemIndex);
    uint16_t getSelectedItem();


    ScrollWheelWithSelectionStyle &getWheel();
    MenuItemSelected *getSelectedItem(int16_t itemIndex);
    MenuItemNotSelected *getNotSelectedItem(int16_t itemIndex);

protected:
    virtual void wheelUpdateItem(MenuItemNotSelected &item, int16_t itemIndex) override;
    virtual void wheelUpdateCenterItem(MenuItemSelected &item, int16_t itemIndex) override;

    touchgfx::Callback <Menu> mAnimationEndCb;
    void scrollWheelAnimationEndCb();

    MenuItemSelected mSelItems[skMaxNumberOfItems] { };
    MenuItemNotSelected mNotSelItems[skMaxNumberOfItems] { };
};

#endif // MENU_HPP
