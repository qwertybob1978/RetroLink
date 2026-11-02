#include "designer_form.h"

void RetroLinkInitialize(DialogPtr form);

int main(void)
{
    return MacStudioRunForm();
}

void MacStudioFormDidLoad(DialogPtr form)
{
    RetroLinkInitialize(form);
}