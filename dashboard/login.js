document.getElementById('loginForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    
    const email = document.getElementById('email').value;
    const password = document.getElementById('password').value;
    const errorMsg = document.getElementById('errorMessage');
    const loginBtn = document.getElementById('loginBtn');
    
    errorMsg.textContent = '';
    loginBtn.innerHTML = '<i class="fa-solid fa-spinner fa-spin"></i> Autenticando...';
    loginBtn.disabled = true;

    try {
        const response = await fetch('http://localhost:3000/api/auth/login', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({ email, password })
        });

        const data = await response.json();

        if (!response.ok) {
            throw new Error(data.error || 'Erro ao autenticar');
        }

        // Sucesso: Salvar token e dados no LocalStorage
        localStorage.setItem('kiira_token', data.token);
        localStorage.setItem('kiira_user_role', data.role);
        localStorage.setItem('kiira_user_email', data.email);

        // Redirecionar para o painel de operação
        window.location.href = 'index.html';
        
    } catch (error) {
        errorMsg.textContent = error.message;
        loginBtn.innerHTML = 'Autenticar <i class="fa-solid fa-arrow-right-to-bracket"></i>';
        loginBtn.disabled = false;
    }
});
